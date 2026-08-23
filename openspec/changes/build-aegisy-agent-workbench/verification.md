# Verification Plan

## OpenSpec

- Run `openspec validate build-aegisy-agent-workbench --strict`.
- Confirm every capability listed in `proposal.md` has one delta spec or an
  explicitly retained/deferred implementation source.
- Confirm every requirement has at least one four-hash Scenario and normative
  SHALL/MUST language.
- Confirm `tasks.md` is fully machine-trackable through numbered unchecked boxes.

## Task Ledger Status Correction (2026-08-07)

- An evidence audit corrected 24 checked rows covering 20 unique task IDs back to
  unchecked: `2.3`, `2.7`, `3.6`, `3.10`, `5.1`, `5.2`, `9.6`, `9.8`, `11.9`,
  `12.1` through `12.10`, and `14.5`. Their existing notes or verification gates
  explicitly retain incomplete, pending, deferred, or keep-unchecked work.
- The 2026-08-23 scope reset adds eight unique section-0 rows, with the product
  decision complete and seven delivery gates open. The current raw baseline is 78
  complete and 169 unchecked across 247 rows. Task IDs `12.5`, `12.7`, and `12.8`
  remain duplicated, so the unique-ID baseline is 78 complete and 165 unchecked
  across 243 tasks.
- This is a task-status correction, not a functional regression or code rollback.
  Existing partial implementation and verification evidence remains intact; each
  task may be checked again only after all of its recorded completion gates pass.

## Architecture Evidence Required Before Implementation

- UI spike report comparing Qt WebEngine and standalone Tauri fallback.
- AAP generated-schema round-trip across Rust, TypeScript, and C++.
- Authenticated IPC hostile-client test report.
- Codex App Server and ACP adapter fixture report.
- macOS and Windows sandbox feasibility report.
- Model catalog/schema and scoped-token backend review.
- Third-party license, attribution, and redistribution review.

## AAP Schema Package (`3.1`)

- `agent-runtime/aap-schema/package.json` identifies a private checked-in Schema
  package and points to exactly one stable and one experimental namespace registry.
  Package, wire-protocol, and provider-adapter versions remain independent.
- The stable registry binds AAP `0.1`, directory `v0.1`, the ordinary package-local
  Schema path, and its canonical HTTPS `$id`. The experimental registry is
  deliberately empty, compatibility-free, and `wire_available:false`; it creates
  no experimental negotiation or method authority.
- `schema_package` validates exact package/registry shapes, package-contained
  non-symlink paths, unique namespaces/versions/Schema IDs, directory/version/`$id`
  agreement, stable Schema compilation, and the absence of stable references to
  experimental. The focused gate passes 3/3; `handshake_schema` passes 23/23;
  the complete Rust workspace passes 977 tests with one explicitly ignored live
  Codex fixture; strict workspace Clippy and formatting checks pass. The complete
  desktop build and CTest suite pass 20/20, strict OpenSpec validation passes, and
  all package/namespace/stable Schema JSON documents pass `jq empty`.
- The package-structure evidence alone does not claim core domain definitions or
  generated types. Core evidence is recorded separately below; generated
  Rust/TypeScript/C++ types remain required under `3.10`.

## AAP Core Domain Schemas (`3.2`)

- Stable AAP `0.1` keeps its original transport `schema/schema_id` registry entry
  and additively registers `core.schema.json` as the `core-domains` component.
  The core component is a `$defs` library rather than a fabricated all-domain
  wire message.
- The definitions cover thin Project/Session, project-root/navigation and durable
  Session projections, separate Turn lifecycle and start acknowledgement states,
  transport-aligned Item plus history Item, live/search/replay Runtime variants,
  project-root Workspace Git state, non-authorizing Approval acknowledgement,
  content-free Error, typed Usage, command-output Artifact read, and negotiated
  Capability sets. Three-language generated types remain open under `3.10`.
- `aap-core-domains.json` is a named positive fixture catalog. The package gate
  compiles individual `$defs`, checks core/transport Item parity, and rejects
  unknown fields, Chat/Work and Turn-state drift, contradictory Workspace Git
  state, false-authority changes, Provider body/credential fields, unsafe or
  inconsistent Usage, generic Artifact identity, duplicate/experimental
  capabilities, and invalid identities. It also preserves every public core enum
  value and security-relevant bound as an additive compatibility baseline,
  cross-checks the 64-character Runtime/Backend handshake names, and requires live
  and history Item data keys to share the exact ASCII graphical boundary. The
  focused gate passes 12/12.
- `core_schema_runtime` captures real Store-backed `project/open`,
  `project/root-list`, `project/list`, Chat/Work `session/start`, `session/list`,
  `session/search`, cold `session/read`, `turn/start`, terminal Turn projection,
  and Timeline Items. It validates each value against its exact definition and
  validates Timeline Items against both core and transport; the focused gate
  passes 1/1.
- Runtime producer fixes validate the final Session title before backend dispatch,
  preserve non-empty filesystem identity in memory-only and Store-backed project
  listings/root projections, reject same-path directory replacement, and enforce
  JSON-safe integers throughout Usage reports, evidence, values, and provider
  ingestion. Replacement fixtures keep the old filesystem object alive so inode/
  file-index reuse cannot weaken the evidence. Focused title/project tests pass 3/3
  and Usage tests pass 16/16. JSON Schema `maxLength` remains character-based;
  typed UTF-8 byte and cross-field validation is still required.
- Focused commands:

  ```sh
  jq empty agent-runtime/aap-schema/stable/namespace.json \
    agent-runtime/aap-schema/stable/v0.1/core.schema.json \
    agent-runtime/aap-schema/fixtures/aap-core-domains.json
  $HOME/.cargo/bin/cargo test --manifest-path agent-runtime/Cargo.toml \
    -p aegisy-agentd --test schema_package
  $HOME/.cargo/bin/cargo test --manifest-path agent-runtime/Cargo.toml \
    -p aegisy-agentd --test core_schema_runtime
  ```

- This local macOS evidence grants no Agent write, execution, Git, background,
  user Approval, generic Artifact, experimental, dedicated-worktree, remote, or
  Windows release authority.
- Final local gate: 44 `aegisy-aap`, 785 `aegisy-agentd` library with one
  explicitly ignored live Codex fixture, 7 daemon, 10 context-threshold, 1 real
  core-Schema Runtime capture, 21 handshake Runtime, 23 handshake Schema, 68
  protocol, 12 Schema-package, and 23 stdio/Codex tests pass (994 passed total,
  zero failed, one ignored). Strict Clippy and Rust formatting pass; the complete
  desktop build and all 20 CTests pass; all registered JSON documents parse;
  strict OpenSpec validation and `git diff --check` pass.

## AAP Core Generated Types (Partial `3.10`)

- `generate-core-types.mjs` reads the registered `core.schema.json` and emits
  checked-in Rust, TypeScript declaration/runtime, and Qt/C++ header/source
  artifacts. Its Schema-AST inventory rejects unknown keywords, unsupported
  dialects and semantic combinations, unresolved references, and fractional
  number schemas instead of generating a weaker validator.
- Generated decoders use strict object shapes and definition-level validation.
  Rust, TypeScript, and Qt/C++ all enforce JSON-safe integers plus the typed Item
  `data` boundary of at most 16 levels and 4,096 aggregate values.
- `aap-core-domains.fixture-map.json` binds every positive fixture key to its
  definition and records the reviewed canonical catalog identity. Each language
  independently decodes and canonically serializes the complete catalog; the CMake
  verifier rejects any byte-count or SHA-256 difference.
- `aap-core-generated-corpus.json` is a separate, shared 43-case accept/reject
  corpus. It includes valid boundaries and rejection cases for unknown fields,
  identifier and union drift, false authority, conditionals, Usage consistency,
  Item property names/fractions/safe-integer overflow, ItemData depth/node
  boundaries, lone-surrogate DTO strings, ItemData strings and ItemData keys,
  Workspace state, and duplicate/experimental capabilities. The materializer is
  bounded and exact-keyed and emits raw `value_json`; each language independently
  parses every candidate, hashes the ordered `name`, `definition`, and
  `accept|reject` decisions, and compares the reviewed golden.
- CMake configures `aap_generated_types` only with explicit Node, Cargo, and the
  Qt/C++ runner. The runner uses `QCoreApplication::arguments()` for platform-native
  Unicode paths. `.gitattributes` fixes the Schema package, generated artifacts,
  and verifier sources to LF. The Rust output resides in the `aegisy-aap` crate so
  `cargo package` can verify that no source-tree-relative generated include is used.
- Focused commands are:

  ```sh
  node agent-runtime/aap-schema/scripts/test-core-generator-inputs.mjs
  node agent-runtime/aap-schema/scripts/generate-core-types.mjs --check
  cmake --build build --target AegisyAapGeneratedTypesTest -j4
  ctest --test-dir build -R '^aap_generated_types$' --output-on-failure
  cargo package --locked --offline --manifest-path agent-runtime/Cargo.toml \
    -p aegisy-aap --allow-dirty
  ```

- The focused macOS run passes the generator negative-input suite, generated-file
  freshness check, `AegisyAapGeneratedTypesTest` build, and `aap_generated_types`
  CTest (1/1). Rust, TypeScript, and Qt/C++ each emit the same positive fixture
  identity, `9801 94a27009b9c2439cef5a31f3078eacd3265df24a47c75bffb0c59d75f87d7f11`,
  and the same corpus identity,
  `43 4d606e318e836e001f1cf9ec69d8b5ff558cc76158ed61c9862b92e4399b94ce`.
  `cargo package -p aegisy-aap --allow-dirty` packages seven files and verifies
  the packaged crate by compiling it. Strict OpenSpec validation and
  `git diff --check` also pass for this stage.
- This evidence does not complete task `3.10`. Generation from
  `aap.schema.json` is now covered by the Transport slice below, but migration of
  the remaining hand-written Rust/Qt consumers and execution from a clean Windows
  Unicode checkout remain absent. The partial generated domain layer grants no new
  capability, permission, mutation, Approval, or execution authority.

## AAP Transport Generated Types (Partial `3.10`)

- `generate-transport-types.mjs` reads stable `v0.1/aap.schema.json` and emits
  checked-in Rust, TypeScript declaration/runtime, and Qt/C++ header/source types.
  Negative generator fixtures reject dialect/ID drift, unknown keywords and refs,
  unsupported types, unsafe bounded integers, normalized type/field/enum collisions,
  and by-value dependency cycles. Dependency-topological declaration ordering keeps
  the generated C++ header independently compilable.
- The strict raw parser profile is identical across the Node oracle, Rust, and C++:
  4 MiB frame, depth 128, 65,536 JSON nodes, valid UTF-8 and Unicode scalars, no
  leading BOM, no duplicate decoded keys, and exact arbitrary-precision number
  lexemes. Mathematical integers such as `1.0` and `1e0` validate as integers.
  Canonical JSON sorts keys by UTF-8 bytes and normalizes numbers without `double`.
- `aap-transport-methods.json` exactly binds every stable root method condition,
  typed request/success/error/notification definitions, response dispatch, and the
  generic request/notification fallbacks. The materializer validates every binding
  against the root Schema before accepting the fixture catalog.
- All 101 definitions independently validate and serialize to fixture identity
  `29903 d2961275431323f968bd18c4d8c2535cb8b05bda003ff0dea97f6e73be124757`.
  The shared 72-case positive/negative parser and Draft 2020-12 subset corpus has
  decision identity
  `72 f0ce6bdc14c815b2b80b273126da8b20a80ec47371d39128c7e2155246f60404`.
  The materializer, independent Node oracle, generated TypeScript, Rust, and
  generated Qt/C++ public APIs reproduce the applicable identity exactly.
  TypeScript also checks the public lexical/canonical/integer number view, canonical
  serialization of a returned value, rejection of a structurally similar object,
  and rejection of a copied runtime object that lacks the private parser brand.
- CMake registers `aap_transport_runtime` and `aap_transport_generated_types`.
  Both C++ targets compile with `/W4 /WX` or `-Wall -Wextra -Werror`; the aggregate
  gate runs generator negative/freshness tests, oracle, materializer, and all three
  generated-language runners. The exact Schema-package test requires both production
  Runtime paths in `package.json.files`; the automated package-inventory gate runs
  `npm pack --dry-run --json` and requires the exact 47-file inventory, including
  generated C++ plus the `aap_transport_runtime.h`/`.cpp` dependency pair while
  excluding C++ test sources. Focused commands are:

  ```sh
  npm --prefix agent-runtime/aap-schema run generate:check
  npm --prefix agent-runtime/aap-schema run test:generator
  npm --prefix agent-runtime/aap-schema run test:transport:oracle
  npm --prefix agent-runtime/aap-schema run test:transport:typescript
  cmake --build build --target AegisyAapTransportRuntimeTest \
    AegisyAapTransportGeneratedTypesTest -j4
  ctest --test-dir build \
    -R '^aap_transport_(runtime|generated_types)$' --output-on-failure
  ```

- The latest macOS gate passes the generated-language fixtures, strict Clippy and
  formatting, `aegisy-aap` packaging, the complete desktop build, and all 23 CTests
  (including the generated Qt consumer, canonical error-code, environment, and
  Workbench render coverage). Strict OpenSpec validation and `git diff --check` also
  pass. A previous concurrent Cargo run hit a Git fixture timeout; the isolated
  protocol rerun passed, and the final serial 23/23 CTest run passed.
- Keep `3.10` unchecked. Production Rust and Qt consumers now use the generated
  lossless Transport path, parsed dispatch, indivisible pending contexts, and safe
  projection. The Windows validation job now checks out directly under
  `windows-验证-源码`, proves the path is non-ASCII and the Git state is clean,
  builds the complete Release target graph, runs the unfiltered CTest suite, and
  verifies the locked `aegisy-aap` package offline after dependency resolution.
  The Transport Runtime and Artifact Manifest verifier consume native
  `QCoreApplication::arguments()` paths, and their local fixtures now force Chinese
  schema/manifest directories. The complete desktop build and all `25/25` serial
  CTests pass, including those focused Unicode-path gates. Locked offline Cargo
  package, workflow YAML parsing, strict OpenSpec validation, and `git diff --check`
  also pass locally.
  The repository-owned Windows policy test now checks the complete 18-entry trigger
  set, requires exactly one unfiltered CTest invocation, and requires the installer
  upload to use the absolute `${{ github.workspace }}/windows-验证-源码/...` path.
  It constructs three in-memory negative workflows and rejects a removed
  `agent-runtime/**` trigger, an appended CTest `-R` filter, and a relative artifact
  path. The workflow is fixed to LF by `.gitattributes`; a generated CRLF copy must
  independently pass the same validator. The direct script and focused CTest pass;
  this remains configuration evidence rather than Windows execution evidence.
  A clean Windows runner has not yet executed this expanded job, so this remains
  configuration and macOS evidence only and `3.10` stays unchecked.
  Stable `0.1` remains lossless rather than silently narrowing generic numbers; the
  slice grants no capability, permission, Approval, mutation, execution,
  experimental, remote, or Windows release authority.

## Product Baseline Decision Evidence (`1.3`, `1.7`, `1.8`)

- `docs/adr/README.md` maps all nine current `design.md` Open Questions to one
  versioned ADR each. The register and ADR headers agree on one accountable owner,
  consulted owners, closed status vocabulary, and due gates. Model-catalog closure
  includes schema, authority, signing, refresh, scoped-token, cache, and admin
  validation tasks; local-provider and enterprise-retention behavior each require a
  dedicated accepted follow-up OpenSpec rather than borrowing an unrelated task.
- `design.md` links each Open Question to the matching ADR. Accepted decisions may
  be relied upon only within their stated scope; Provisional, Proposed, and Deferred
  entries preserve every named security/release gate and do not fabricate Codex
  redistribution, model-catalog authority, Windows sandbox support, or a public name.
- `docs/AEGISY-SUPPORTED-PLATFORM-MATRIX.md` distinguishes Qt 6.8 capability from
  repository build, CI/clean-machine evidence, and Aegisy release support. It records
  the actual macOS deployment target and Windows x64 workflow, filesystem/shell/Git
  assumptions, display/IME/accessibility rows, unsupported targets, evidence owners,
  and promotion requirements. A local macOS pass or Windows compile cannot promote a
  row without the exact signed-package evidence.
- `docs/AEGISY-WORKBENCH-FEATURE-CHANNEL-POLICY.md` defines internal/preview/beta/
  stable maturity, local versus separately gated remote surface, and subtractive
  emergency revocation. It specifies fail-closed authority intersection, feature
  registry fields, promotion/rollback/persistence/audit contracts, remote denial,
  emergency `0.1` limits, and table/property/parity/race/signed-package verification.
  This completes policy task `1.8`, not the production rollout registry, remote
  architecture, emergency publisher/secure anchor, or release criteria under
  `22.6`, `22.9`, and `22.10`.
- Verification commands: check every relative Markdown link, ensure each ADR status
  is one of the registered values, count exactly nine Open Question ADR links, run
  `openspec validate build-aegisy-agent-workbench --strict`, and run
  `git diff --check`.
- Git product-version enforcement evidence: `GitRunner::new` resolves the absolute
  executable outside the project, runs a 256-byte-capped `git --version` check with
  a sanitized environment and a five-second bounded wait, accepts the documented
  Git/Linux, Apple Git, and Git for Windows formats, and returns the stable
  Git-unavailable code below `2.31.0`
  or for malformed/failed/oversized output, including cleanup after timeout or
  read failure. The `.gitignore` compatibility path is
  intentionally isolated from this product gate. The `git_status::tests` focused
  run passes 9/9, including real executable fixtures for `2.30.9` rejection,
  non-zero exit, malformed output, oversized output, and timeout kill/reap, plus
  `2.31.0` boundary acceptance, Apple/Git-for-Windows parsing, installed-Git
  preflight, and repository status regressions. This does not replace clean
  Windows runner or signed-package evidence required for release support.
- Windows packaging policy evidence: `installer.iss` declares the Windows 10 1809
  technical floor as `MinVersion=10.0.17763`; the generated application resource
  embeds the requested `asInvoker` execution level and a
  `longPathAware=true` manifest setting. The cross-platform
  `windows_packaging_policy` CTest passes and checks all three source files.
  This is a source-policy gate only; clean Windows OS long-path behavior, TLS,
  installer execution, signing, and upgrade evidence remain required.

## Milestone Evidence

Each milestone in `roadmap.md` must attach:

- Automated build/test/security/evaluation output.
- Signed package smoke-test matrix for claimed platforms.
- Failure-injection and recovery results.
- Performance against predeclared budgets.
- Data/privacy review for new persisted or transmitted fields.
- Screenshots or recordings for responsive, IME, accessibility, approval, error,
  and recovery states.
- Known limitations, rollback path, and feature-flag state.

## Current Implementation Verification

Implementation is in progress and does not claim that the full workbench, model
catalog, sandbox, or autonomous feature set is complete.

AAP 0.1 initialization and capability-negotiation evidence (`3.3`):

- The stable schema, Rust types/Runtime, daemon stdio loop, Qt client, checked-in
  fixtures, internal guide, design, and protocol delta spec agree on one strict
  two-stage contract: `initialize` carries structured client version range,
  identity, platform, stable/experimental capability declarations, fixed frame
  limit, and current transport-security facts; the client becomes usable only
  after the Runtime consumes the exact object-params `initialized` notification.
- Protocol versions are compared as bounded `(u64, u64)` pairs rather than text.
  Both incompatible directions return a content-free `initialize-error/0.1`
  with the exact `client` or `runtime` upgrade direction and leave the Runtime
  available for a corrected initialize request.
- The Runtime returns only the declared/available stable capability intersection
  in deterministic registry order and exactly an empty experimental list. Ready
  or recovery state requires the matching backend marker and
  `permission.read-only`; every ordinary method is denied until the second-stage
  notification and when its stable capability was not negotiated. Terminal stop
  requires lifecycle, platform, and out-of-band capabilities together.
- Requests and notifications require object `params`; success responses require a
  non-null bounded ASCII graphical string ID, while parse/envelope errors may use
  `id:null`. Queue-full handling applies the same strict envelope classification.
  The Draft 2020-12 schema tests validate both compatible/incompatible fixtures and
  reject legacy handshake, null success IDs, empty stable declarations, non-empty
  experimental declarations, and missing request/notification params.
- AAP `0.1` fixes both directions at exactly 4 MiB. The daemon drains a physical
  oversized line without parsing it, returns fixed content-free `id:null/-32005`,
  and accepts the next frame. Oversized responses become same-ID `-32005` before
  writing; oversized notifications close the transport with zero output instead of
  being silently dropped. Qt rejects oversized outbound requests before adding
  pending state and terminates on oversized or malformed inbound protocol data.
  Tests prove a credential-shaped oversized body is neither parsed nor echoed.
- The current local stdio transport truthfully reports `local:true` with
  authentication, encryption, and peer verification all false. Authenticated Unix
  sockets and Windows named pipes remain owned by `4.2` through `4.4`; no transport
  authority, credential field, or Agent mutation permission was added by `3.3`.
- Verified commands on 2026-07-24:
  `cargo fmt --all --manifest-path agent-runtime/Cargo.toml -- --check`;
  `cargo test --workspace --manifest-path agent-runtime/Cargo.toml` (8 AAP type,
  630 sidecar library passed plus one ignored live fixture, 6 daemon main, 10
  context-threshold, 13 handshake Runtime, 5 handshake Schema, 63 protocol, and 22
  stdio/Codex tests); strict workspace Clippy with warnings denied; CMake builds for
  `AegisyAgentRuntimeEnvironmentTest` and `AegisyAgentWorkbenchRenderTest`; CTest
  `agent_runtime_protocol`, `agent_workbench_render`, and
  `agent_runtime_environment`; strict OpenSpec validation; and `git diff --check`.

AAP 0.1 ordered Timeline event evidence (`3.4`):

- Stable `timeline-event/0.1` envelopes now bind a Session-local positive contiguous
  sequence, non-decreasing positive millisecond timestamp, exact Session/Turn
  correlation, explicit running/completed/failed/interrupted Turn state, optional
  Item snapshot, and positive contiguous Item revision using
  `snapshot-replacement`. Rust, JSON Schema, Qt, fixtures, the internal guide,
  design, and protocol delta spec share the same field and lifecycle contract.
- Event IDs are independently reproducible in Rust and Qt as SHA-256 over the fixed
  domain, unsigned 64-bit big-endian canonical JSON byte length, and compact UTF-8
  canonical JSON. Envelope/Item fields use fixed order; data-object keys sort by
  UTF-8 bytes; arrays retain order; nullable fields remain present. Mathematical
  JSON integers such as `1.0`, `1e3`, and `-0.0` normalize to the shortest decimal
  integer before hashing, while fractional and out-of-safe-range values fail closed.
- The Runtime sequencer owns Turn and Item lifecycle transitions. Known event
  shapes, generic 64-byte Item kinds, stable kind/role identity, revision order,
  open-Item terminal rejection, duplicate/late terminal rejection, timestamp
  rollback, and sequence exhaustion are validated before cursor mutation. Failed
  sequencing advances no sequence or Item revision. If a terminal live event cannot
  be sequenced after durable terminal persistence, the Runtime disables the backend
  and closes stdio without emitting an invalid event; durable history remains intact.
- Qt validates and advances cursors, Turn state, and Item state independently for
  every Session, including A/B/A interleaving. Background Session events never
  render into or alter the visible Session UI. Unknown itemless running events
  advance only their Session cursor and produce bounded content-free diagnostics.
  Structured `turn.failed` retains accepted `started`/`delta` partial Items and adds
  its validated error Item before closing the Turn. Completed and interrupted Turns
  continue to reject those open streams, but may retain repeatable `updated`
  metadata snapshots; revision-one atomic `truncated` markers are also accepted.
  Regression coverage proves the terminal cursor advances exactly once and that no
  later Item, terminal, or reopen event mutates a failed Turn.
  Tests cover generic Item lifecycles, kind/role drift, 3 MiB structured data,
  exact 4 MiB notification framing, `4 MiB + 1` rejection, mathematical integer
  normalization, and Rust/Qt Event ID agreement.
- Verified commands on 2026-07-24: Rust formatting; full workspace tests (17 AAP
  type tests, 644 sidecar library tests passed plus one ignored live fixture, 6
  daemon-main, 10 context-threshold, 13 handshake Runtime, 12 handshake Schema,
  63 protocol, and 23 stdio/Codex tests); strict workspace Clippy; all 16 desktop
  CTests; strict OpenSpec validation; and `git diff --check`.
- This historical `3.4` stage did not claim durable public Timeline journaling,
  snapshot/replay, subscription, heartbeat, reconnect catch-up, or gap recovery.
  The later `3.5` sections and current project memory supersede that status; the
  current Agent/Codex permission boundary remains read-only.

Public Timeline fixed-watermark replay slice (`3.5`, partial):

- The connection-liveness slice defines exact AAP
  `runtime-heartbeat-request/0.1` and `runtime-heartbeat/0.1` contracts behind
  `runtime.heartbeat.out-of-band`. The only request data is a non-empty bounded ASCII
  graphical nonce; the exact response echoes it with `state=alive`. Stable Schema,
  Rust type, handshake, and fixture tests reject notifications, extra fields,
  invalid schema/state/nonce, missing negotiation, duplicate IDs, and ordinary-path
  dispatch. The result contains no time, PID, Store/Provider state, permission, or
  execution authority.
- A real stdio saturation fixture blocks a Turn, fills the ordinary 32-request queue,
  and proves heartbeat still returns through the control reader while every stdout
  producer shares the complete-frame writer mutex. Qt fake-runtime coverage proves
  healthy repeated single-flight probes, 5-second/15-second production timing,
  Unknown without `connectionStateChanged(false)`, ordinary pending failure and new
  request denial, continued terminal Stop, inert late/old-generation replies, and
  liveness restoration only after a fresh handshake. The Workbench render fixture
  proves Unknown preserves the active Turn and its Stop action while changing the
  status surface. The Windows packaging workflow runs the same Qt Runtime environment
  test before installer construction.
- At this historical heartbeat-only gate, the slice did not implement bounded
  sidecar reconnection, live Timeline subscription, explicit acknowledgement, or the
  race-free subscribe/sync/activate reconnect flow. The later bounded-reconnect and
  live-subscription sections supersede those first two gaps. Heartbeat Unknown does
  not kill a possibly live Turn, automatic Timeline pruning remains disabled, and
  explicit acknowledgement plus Windows evidence remain open.

Bounded reconnect barrier and OOB handshake ordering slice (`3.5`, partial):

- Qt reconnect barriers now retire their dedicated request IDs on success, malformed
  response, and explicit failure. `session/read` continues Timeline sync only after a
  valid response and freezes the affected Session on failure. Terminal `list` and
  `attach` require exact Session/terminal/generation bindings; if the new output
  cannot be verified, the prior output remains visible with an unverified state and
  no exited inference. Latest Proposal revalidation handles success, empty result,
  drift, invalid response, and request failure without allowing an untrusted response
  to replace a validated cache. `runtime/degradations` records the request ID at
  creation and accepts only the exact matching response, isolating late responses
  across handshake generations.
- Runtime's independent control reader now queues heartbeat, cancellation, steering,
  and terminal-stop messages received before `initialized`. The main dispatcher
  consumes the handshake barrier and re-dispatches the queued messages through the
  normal out-of-band router, preserving ordering while keeping controls reachable
  during ordinary queue saturation. A Rust handshake regression fixture covers the
  race.
- Focused Qt `agent_workbench_render` and `agent_runtime_environment` tests pass
  2/2; the complete desktop suite passes 16/16 CTests. The complete Rust workspace
  passes 28 AAP type, 729 `aegisy-agentd` library (one ignored live fixture), 7
  daemon-main, 10 context-threshold, 14 handshake Runtime, 18 handshake Schema,
  67 protocol, and 23 stdio/Codex tests. Strict Clippy, formatting, successful
  `cmake --build build -j4`, strict OpenSpec validation, and `git diff --check` pass.
- At this historical bounded-reconnect gate, the slice did not yet implement live
  subscription, explicit acknowledgement, or the race-free subscribe/sync/activate
  state machine. The later live-subscription section supersedes that protocol gap.
  Windows reconnect/runtime execution evidence and explicit acknowledgement remain
  absent, and automatic pruning remains disabled. Keep task `3.5` unchecked.
- Generation-bound regression coverage also proves initialize response retirement,
  inert duplicate late initialize responses, exact heartbeat deadline request/process
  generation matching, and process-generation-bound reconnect stability timers.

Historical non-routable Timeline subscription contract foundation (`3.5`, partial):

- Stable Schema `$defs` and Rust types define strict subscribe, activate, live-event,
  and failure objects while deliberately leaving the top-level AAP method schema,
  negotiated capabilities, Runtime dispatch, and Qt client unchanged.
- `sync-required` requires one non-null fixed watermark and `snapshot-required`
  requires a null watermark. Subscribe cannot return active or failed inline.
  Activation must match a private non-serializable structural recovery proof. Sync
  proof creation validates the complete 10,000-Event/64 MiB-bounded page chain from
  the subscribed cursor to the exact watermark; Snapshot proof creation validates fixed headers, contiguous
  pages, ordered complete Items, and the domain-separated complete snapshot identity.
  A merely well-formed anchor or snapshot identity cannot produce verified recovery.
  The proof is not connection authority; the later Runtime must also consume the
  connection-owned registry token for the exact attempt.
- Failure validation fixes subscribe, sync, snapshot, activate, and live stages to
  their exact connection generation, Session, subscription, cursor, watermark, and
  a domain-separated identity of the complete typed request or exact active state.
  Snapshot identity, continuation cursor, and page limit drift therefore cannot
  reuse a late failure. Every accepted failure remains terminal and requires cleanup.
- This historical slice did not bind ordinary `timeline/sync` or `timeline/snapshot` page
  requests to a connection-owned subscription attempt, implement Runtime atomic
  registration/head capture or activation/buffer drain, register a wire method,
  handle event-before-activate in Qt, provide explicit acknowledgement, or supply
  Windows reconnect/runtime evidence. The live subscription stage below supersedes
  the Runtime/Qt gaps without supplying acknowledgement or Windows evidence.
  Automatic pruning remains disabled and task `3.5` stays unchecked.
- Verification passes the complete Rust workspace: 37 AAP type, 729
  `aegisy-agentd` library tests with one ignored live fixture, 7 daemon-main, 10
  context-threshold, 14 handshake Runtime, 20 handshake Schema, 67 protocol, and 23
  stdio/Codex tests (907 passed, zero failed, one ignored). Strict workspace Clippy,
  Rust formatting, JSON Schema parsing, strict OpenSpec validation, and
  `git diff --check` pass.

Live Timeline subscription and ownership recovery (`3.5`, partial):

- Stable Schema, Rust, and Qt negotiate `timeline.subscription.fixed-watermark` and
  route subscribe, subscription-bound Sync/Snapshot, activation, live events, and
  typed terminal failures. Runtime advertises the capability only with a healthy
  writable Store, binds every attempt to one connection generation and Session,
  captures a fixed durable head, and never reuses an ID in that generation. Retained
  Sync/Snapshot recovery units and buffered live events share one connection-wide
  10,000-unit/64 MiB budget with exact single release on every terminal path. A
  cross-Session or cross-generation request is rejected without retiring the true
  owner. Activation consumes the exact private registry token before draining events.
- Negotiated `turn/start` requires the Session to own a valid attempt. Qt enables a
  new Turn only after the exact activation response, rejects stale generation/
  request/Session traffic, preserves confirmed state and queued input across typed
  failure, and retries eligible typed failures with fresh IDs at `0/250/1000 ms`.
  Heartbeat ambiguity, locally invalid subscription state, Active wrapper/cursor
  drift, unsafe continuation, and occupied/reused subscribe identities replace the
  Runtime generation; they cannot falsely complete reconnect or retry on the same
  ownership-ambiguous connection.
- The complete gates pass: 38 AAP type, 753 `aegisy-agentd` library tests with one
  ignored live fixture, 7 daemon-main, 10 context-threshold, 21 handshake Runtime,
  22 handshake Schema, 67 protocol, and 23 stdio/Codex tests (941 passed, zero
  failed, one ignored); strict workspace Clippy and formatting; desktop build and
  all 16 CTests; JSON Schema parsing; strict OpenSpec validation; and
  `git diff --check`.
- This live-subscription slice does not provide complete Windows reconnect/runtime
  evidence. Durable Turn-start acknowledgement is verified by the separate slice
  below. Agent/Codex remains read-only, automatic Timeline pruning remains disabled,
  and task `3.5` stays unchecked.

Durable Turn-start acknowledgement (`3.5`/`3.6`, partial):

- Schema v20 adds the strict `mutation_acknowledgements` ledger and v19-to-v20
  migration/backup path. The stable AAP contract registers
  `session.mutation-acknowledgements` and `mutation/acknowledgement/consume` behind
  `session.mutation-acknowledgements`; pages are Session-scoped, bounded, and
  cursor-validated.
- `turn/start` reserves one metadata-only `turn-start` operation before dispatch,
  keyed by Session, idempotency key, and request fingerprint. Equivalent retries
  return the original operation/Turn; conflicting fingerprints fail without a
  second dispatch. Accepted `turn.started` and terminal Timeline anchors bind in
  the same projection/Journal transaction with revision CAS. Startup converts
  accepted operations with uncertain dispatch to `reconciliation-required`, which
  cannot be redispatched.
- Qt consumes only after validating the exact Session, Turn, sequence, Event-ID,
  and revision anchor; accepted evidence is consumed before terminal evidence.
  Drift, cross-Session access, malformed/tampered rows, unavailable/read-only Store,
  and failed recovery freeze the affected Session for reconciliation. The operation
  contains no prompt, context, provider body, result content, permission, approval,
  or execution authority.
- Focused evidence passes 5 durable acknowledgement tests and 169 Workbench Store
  tests. Preview reservation/binding rollback and retry, startup reconciliation/no
  redispatch, accepted/terminal CAS binding, confirmed-anchor consumption,
  cross-Session rejection, tamper detection, read-only recovery, purge, and the
  schema migration are covered by the implementation tests. Approval, file-write,
  Git, and job mutation producers remain absent, and complete Windows
  reconnect/runtime evidence remains open; keep `3.5` and `3.6` unchecked.

Historical schema-v21 metadata-only mutation foundation and current server-request
contracts (`3.6`/`3.7`, partial):

- The historical v21 `mutation_reservation.rs` foundation projected approval, file-write,
  Git-mutation, and background-job request contracts into strict content-free
  `mutation-reservation-draft/0.1` records. Domain-separated identities bound the
  source schema/kind/operation/scope plus Session/project/root/Turn, idempotency,
  and normalized request fingerprint. Retry classification used the intended
  Session/kind/idempotency uniqueness tuple: exact validated equality replayed,
  binding drift conflicted, and different keys or kinds were unrelated. Three
  focused tests covered all four source kinds, retry classification, strict wire
  round-trip, authority forgery, unknown fields, scope drift, and secret shapes.
- Workbench database schema v21 added the separate
  `mutation_reservation_records` wrapper without changing the schema-v20
  `mutation_acknowledgements` Turn ledger or AAP wire. One row stored the exact
  validated draft, redundant source/scope/key/fingerprint columns, its SHA-256,
  state, exact revision, and times. The inner draft fixed schema-v20 and
  Turn-anchor compatibility, `reservation_persisted`, dispatch, mutation,
  Approval, and execution to false; that was a non-authorizing self-declaration,
  while the validated v21 wrapper alone proved that the draft was persisted.
- That v21 Store enforced a unique Session/kind/idempotency tuple. Exact equality while
  `reserved` reused the original row through a read-only preflight without write
  admission, including while low-space policy rejected new writes or another SQLite
  connection held the write lock. For an absent key, the `IMMEDIATE` transaction
  rechecked the database-backed Session deletion state, Session status,
  Session/project/root/Turn scope, and idempotency tuple before insertion. A deletion
  committed between read-only preflight and that transaction returned
  `session-deletion-pending` and left zero reservation rows. Fingerprint or
  source-binding drift conflicted; different keys and kinds remained independent.
  Admission and
  startup verified Session/project/root/Turn scope, redundant column and canonical
  JSON/hash binding, exact two-state revision/time semantics, and Session-owned
  reads. Startup advanced every unresolved row from `reserved` revision 1 to
  `reconciliation-required` revision 2 and a repeated reservation then failed
  closed without advancing again on the next restart. Project-root/Turn foreign keys
  and an explicit root-removal guard preserved scope; pending Session deletion blocked
  retry and final Session purge removed the rows. The table was bounded to 10,000 rows
  globally and 16 KiB per draft; v20-to-v21 migration created an empty reservation
  table, preserved the existing Turn ledger, and recorded a normal migration backup.
  Every source version below 21 checked inside the migration's `IMMEDIATE` transaction
  that the reservation table and both named indexes are absent, using case-insensitive
  names and rejected any SQLite object type. Weak DDL, a case-variant View, either
  index by itself, and exact future-schema DDL with semantically valid rows collided
  and rolled back without advancing `user_version` or changing source objects or rows;
  a v12 fixture additionally proved objects created earlier in the transaction rolled
  back. Schema-v21 startup compared the complete table-bound object inventory with a
  fresh canonical schema, so an extra Trigger or index entered whole-Store read-only
  recovery before reconciliation could execute it. Semantic row tamper or an over-limit
  startup also entered whole-Store read-only recovery.
- Fourteen historical v21 focused Store tests proved all four kinds remained metadata-only and
  authority-free, exact retry and binding-drift conflict, key/kind partitioning,
  projectless Chat Approval plus Session/project/root/Turn scope rejection,
  low-space/write-lock replay, stable second restart, reconciliation retry refusal,
  root-removal and existing pending-deletion gates, the deterministic
  preflight-to-write deletion race with `session-deletion-pending` and zero-row
  preservation, Session purge, SQL UTF-8 byte/read bounds, lifecycle/JSON/hash/
  column/owner tamper rejection, unexpected attached Trigger/index rejection, weak,
  case/type/index-only, v12, and exact future-schema collision rollback,
  empty v20-to-v21 migration, and write/startup row limits. The Rust encoder applies
  the same 16 KiB ceiling independently, while valid source contracts were already
  structurally smaller than it. These records contained no prompt, command, path,
  provider body, result, credential, or permission data.
- The historical v21 local reservation gate passed all 14 focused tests, the complete locked
  Rust workspace (`868` library tests passed and one installed-Codex live fixture
  ignored), strict all-target Clippy, formatting, the desktop build, strict OpenSpec
  validation, and `git diff --check`. All 31 CTests other than the unchanged
  `windows_debug_pipe_policy` passed against the rebuilt sidecar. The unfiltered 32-test
  run failed only that policy fixture because the focused Windows debug workflow's
  Monaco step name and fail-closed exit handling drifted; this is not reservation
  evidence and is not represented as a complete desktop gate.

Schema-v22 complete-source verification matrix. The focused contract run passes
`7/7` `mutation_reservation::tests`, and the focused Store run passes `39/39`
`non_turn_mutation` tests, both with zero failures:

| Gate | Required evidence | Status |
| --- | --- | --- |
| Four typed sources | The 7-test contract run covers canonical round-trip, bounded bytes, hash and identity reproduction, and source-to-draft derivation for `approval-acknowledgement/0.1`, `file-write-acknowledgement/0.1`, `git-mutation-acknowledgement/0.1`, and `background-job-request/0.1` | Passed |
| Lossy-draft conflict | Two complete sources that derive the same draft still conflict on the same Session/kind/idempotency tuple when any source field drifts | Passed |
| Atomic initial graph | Source record, derived reservation, internal `mutation.reservation-source-recorded` event, and internal Session sequence commit together; no Public Timeline row or public-sequence growth occurs | Passed |
| Exact retry | An exact complete-source retry returns the original source/reservation/event graph with zero writes and no Session-sequence advancement under ordinary, low-space, and competing-writer conditions | Passed |
| Failure rollback | Deterministic failure injection at source insert, reservation insert, event insert, sequence update, graph validation, and commit leaves no partial graph and no sequence advancement | Passed |
| Integrity and provenance | Source/provenance/event JSON, hashes, identities, anchors, owners, duplicates, and orphans fail closed; deleting or tampering a `present` graph cannot downgrade it to `legacy-unavailable` | Passed |
| Restart reconciliation | First restart validates the complete `present` graph, then atomically records revision 2 plus exactly one `mutation.reservation-reconciliation-required` event; second restart performs zero writes and leaves the sequence stable | Passed |
| v21 migration | Every preserved valid v21 row becomes `legacy-unavailable` plus `reconciliation-required` revision 2 without a fabricated source record, source event, reconciliation event, or Public Timeline history | Passed |
| Migration admission | v21 schema and every row are validated before copy under the 10,000-row bound; pre-v22 reserved event-kind, operation-ID, Event-ID namespaces and shared `events`/`session_sequences` Triggers roll back; locked `user_version` drift accepts only a completed v22 migration and locked `application_id` drift fails before backup | Passed |
| Migration source binding | A separately pre-opened read-only connection holds one `DEFERRED` snapshot across source identity validation and Online Backup; current-v22 opens skip the write lock, lock timeout is bounded/retryable, and early or pre-copy path replacement is detected through file identity without backing up the substituted database or committing migration | Passed on Unix host; Windows implementation awaits its normal platform gate |
| Schema gates | Every v22 table, index, auto-index, Trigger/attached object, case/type variant, weak DDL, and future-shape collision participates in transactional migration rollback and canonical `sqlite_master` inventory comparison | Passed |
| Lifecycle query plan | `events(session_id, operation_id, sequence, event_kind)` is a covering index and SQLite `EXPLAIN QUERY PLAN`, without `ANALYZE`, selects it for the exact lifecycle lookup | Passed |
| Lifecycle, bounds, and security | Session ownership, pending deletion, root removal, Session purge, row/byte limits, and secret rejection cover both provenance classes; every permission, mutation, Approval, execution, and dispatch authority field remains false | Passed |

Focused commands executed successfully on the macOS host on 2026-08-10:

```bash
$HOME/.cargo/bin/cargo test --manifest-path agent-runtime/Cargo.toml \
  -p aegisy-agentd --lib mutation_reservation::tests
$HOME/.cargo/bin/cargo test --manifest-path agent-runtime/Cargo.toml \
  -p aegisy-agentd --lib non_turn_mutation -- --test-threads=1
```

The complete local slice gate also passes Rust formatting, strict workspace/all-target
Clippy, the complete Rust workspace (`1135` passed and one explicitly ignored
installed-Codex live fixture), `cmake --build build -j4`, all `32/32` CTests, strict
OpenSpec validation, and `git diff --check`.

```bash
$HOME/.cargo/bin/cargo fmt --all --check --manifest-path agent-runtime/Cargo.toml
$HOME/.cargo/bin/cargo test --workspace --manifest-path agent-runtime/Cargo.toml
$HOME/.cargo/bin/cargo clippy --workspace --all-targets \
  --manifest-path agent-runtime/Cargo.toml -- -D warnings
cmake --build build -j4
ctest --test-dir build --output-on-failure
openspec validate build-aegisy-agent-workbench --strict
git diff --check
```

The v22 matrix is internal Store evidence only. It must not add a production
approval/file/Git/job producer, AAP capability or method, Qt surface/recovery flow,
Public Timeline event, outcome/result anchor, consume or caller-CAS route, dispatch,
filesystem write, Git mutation, background submission, or genuine user Approval.
Agent/Codex remains read-only, and `3.6`, `5.1`, and `5.2` remain unchecked.

Terminal non-Turn outcome contract (`3.6`, partial):

- `mutation_reservation_outcome.rs` transparently wraps only the reviewed terminal
  acknowledgement/state type for the exact reservation kind. Approval, file-write,
  and Git acknowledgements must be revision 2 and terminal; background jobs must be
  in a validated terminal state. Requested, accepted, queued, running, and
  reconciliation-required values are not outcomes.
- Every outcome is revalidated against the complete reserved source before bounded
  canonical serialization. Kind drift, idempotency/request/binding drift, a changed
  job request, non-canonical JSON, unknown fields, and forged authority fail closed.
  Canonical bytes are capped at 16 KiB and feed both an exact SHA-256 and a
  domain-separated identity that also binds the complete source identity.
- Seven focused tests pass on `rust:1.97.1-bookworm`; Rust formatting, strict
  package/all-target Clippy with warnings denied, and the locked Release workspace
  build pass. The broader container run reaches `883/885` `aegisy-agentd` library
  tests and retains the same two unrelated Git transaction fixture failures present
  on the base commit:
  `previews_and_commits_only_agent_delta_while_preserving_user_index_and_worktree`
  and `injected_ref_failure_rolls_back_and_external_ref_rewrite_is_preserved`.

```bash
cargo test --locked --manifest-path agent-runtime/Cargo.toml \
  -p aegisy-agentd mutation_reservation_outcome -- --nocapture
cargo clippy --locked --manifest-path agent-runtime/Cargo.toml \
  -p aegisy-agentd --all-targets -- -D warnings
cargo fmt --all --manifest-path agent-runtime/Cargo.toml -- --check
cargo build --locked --workspace --manifest-path agent-runtime/Cargo.toml --release
cargo test --locked --workspace --manifest-path agent-runtime/Cargo.toml
```

Workbench schema v23 now uses this contract as a durable internal outcome anchor.
One immutable outcome row and one metadata-only
`mutation.reservation-outcome-recorded` event commit with the reservation
`reserved` revision-1 to `terminal` revision-2 CAS in one `IMMEDIATE`
transaction. Exact retries and a same-outcome peer commit return the original graph
with zero writes; drift, stale scope, reconciliation/legacy state, event/row/CAS
failure, and semantic tampering fail closed. The focused
`non_turn_mutation` Store matrix passes `48/48`, including all four source/outcome
kinds, atomic rollback, startup validation, v22-to-v23 migration/backup, purge, and
read-only recovery. The outcome graph remains crate-internal and adds no external
caller CAS, consume path, AAP capability or method, Qt recovery flow, production
caller, dispatch, filesystem/Git/job execution, genuine user Approval, or
authority. Agent/Codex remains read-only and task `3.6` remains unchecked.

- `git_mutation_ack.rs` defines `git-mutation-acknowledgement/0.1` without
  executing Git or granting authority. Operation identity is domain-separated and
  binds Session/project/root, mutation kind, idempotency key, request fingerprint,
  and immutable plan identity. Exact retries are replayable, same-key binding drift
  is a conflict, and unrelated keys do not collide. Accepted/Committed/Failed/
  ReconciliationRequired transitions enforce contiguous revision and monotonic
  observation time; uncertain and terminal states cannot be advanced by the
  producer, and terminal states require opaque observation evidence. Three focused
  tests cover retry disposition, lifecycle/reconciliation, and strict wire/secret/
  authority rejection.
- `credential_refresh.rs` defines `credential-refresh-request/0.1` with only
  provider/profile and one-way secure-storage identities. Credential values,
  tokens, network access, refresh authority, and secure-storage APIs are absent.
  Four focused tests cover deterministic bindings, strict metadata serialization,
  lifecycle/reconciliation, and secret-shaped/unknown/authority rejection.
- `extension_elicitation.rs` defines `extension-elicitation/0.1` as a bounded,
  content-free metadata contract. It excludes prompts, forms, URLs, arguments,
  and answers; exact retries, derived request/operation identities, contiguous
  lifecycle, terminal/uncertain handling, and fixed-false decision/permission/
  execution/mutation authority are covered by five focused tests.
- `structured_user_input.rs` defines metadata-only `structured-user-input/0.1`.
  It binds an ordered, bounded set of opaque question/option identities and kinds
  to one Session/Turn/request/idempotency fingerprint, without retaining prompt,
  label, option value, answer, or form content. Cancellation has an independently
  recomputable operation-bound identity, exact replay, and explicit completion
  race semantics; invalid identity substitution, unknown/secret-shaped fields,
  bounds/duplicate drift, non-contiguous revision/time, terminal advance, uncertain
  recovery, and any decision/authority claim fail closed. Five focused tests pass.
- These modules are internal foundations only. Schema v21 remains the historical
  draft-only wrapper, schema v22 adds the durable complete source row and internal
  source/reconciliation events, and schema v23 adds the durable terminal outcome
  row/event plus reservation CAS described above. None of the approval/file/Git/job
  production paths calls this graph. It still has no Public Timeline event,
  consume/external-caller-CAS route, AAP capability or method, Qt route/recovery
  flow, dispatch, filesystem/Git/job execution, genuine user Approval, or
  permission/mutation/approval/execution authority.
  The other server-request contracts remain disconnected from Store, Codex
  server-request handling, secure storage, Git execution, and a user Approval
  issuer. Their tests do not prove a usable question/answer flow, approval,
  credential refresh, elicitation UI, or mutation path;
  tasks `3.6` and `3.7` remain unchecked.

Content references and bounded pages (`3.8`, partial):

- `content_reference.rs` defines strict metadata contracts for content
  references, previews, negotiated inline limits, cursors, and pages. A content
  reference binds one of the reviewed artifact/blob/command/diagnostic/image/
  workspace-edit domains to a lowercase SHA-256 and bounded byte count plus an
  allowlisted MIME type. Preview metadata binds the exact reference and records
  only bounded truncation, line-count, or image-dimension facts.
- Local and peer inline budgets intersect by minimum and never widen authority.
  Page windows are byte-bounded, carry an exact next cursor until the terminal
  page, and derive identities from the reference, window, negotiated limits,
  inline digest, and cursor. Text-only inline previews reject common credential
  shapes; binary/image pages remain reference-only. Four focused tests cover
  reference/MIME/hash bounds, preview binding and dimensions, limit intersection
  plus cursor/page identity, and strict unknown/secret rejection.
- This is a read-only foundation. It does not read or write Blob/filesystem
  content, create an AAP method, grant artifact authority, or replace the existing
  Store transaction and artifact-page gates. Task `3.8` remains unchecked until
  those producers, persistence, generated types, and Qt/AAP evidence are wired.
- `artifact/read-command-output-page` is the first production AAP/Store consumer
  of these contracts. First and continuation requests carry the exact
  Session/Item/reference. In-memory lookup is keyed by `(item_id, reference)` and
  durable lookup by Session/Item/reference, so identical output from two Items stays
  independently bound before and after Runtime restart. The load path re-derives the
  durable reference identity, rechecks kind/owner/metadata/UTF-8/integrity, constructs
  the strict content reference and bounded text preview, and returns one UTF-8-safe
  inline page. The first request intersects caller and local
  item/aggregate limits. The aggregate limit is per response, the one inline page is
  capped by both negotiated limits, and a continuation carries the complete derived
  cursor without renegotiation. Its binding identity includes the Session, Item, and
  complete immutable Artifact metadata; bounded `created_at_ms` is returned so a
  consumer can independently reproduce it. The durable read path independently checks
  owner, MIME, exact metadata keys, redaction/truncation arithmetic, UTF-8, byte count,
  content hash, and the unique omission marker at its canonical head/tail boundary.
  The complete Artifact is hash/length/secret-scanned before slicing, so a complete
  `[REDACTED]` placeholder may cross pages while an unredacted secret cannot evade
  detection across pages. Hash-consistent owner rebinding, cross-Session or cross-Item
  cursors, unknown parameters, invalid/tampered cursors, unsupported limits,
  missing/corrupt durable content, and scalar-splitting limits fail closed. The route reuses
  `artifact.command-output.bounded` and grants no mutation, Approval, execution,
  filesystem, or generic Blob authority.
- Focused command-artifact tests plus the explicit redaction and durable owner-rebinding
  tests cover empty output, UTF-8 boundaries, terminal cursor, continuation
  renegotiation rejection, cursor substitution and owner isolation, durable semantic
  tampering, Runtime restart/Store reload, and the legacy whole-artifact path's stable
  creation-time/Item-ID selection. A fixed unbound `0.1` limit/cursor/page vector proves
  the optional binding did not rewrite legacy identities, and fixed simple plus
  quote/slash/backslash command-binding vectors lock the domain prefix, field order,
  unsigned length framing, exact UTF-8, numeric big-endian, and boolean bytes for
  independent Qt reproduction. A marker-collision fixture proves retained source text cannot create a
  second canonical omission marker. The restart fixture uses a legal `.`/`:` Item ID
  and verifies its deterministic event operation mapping and exact durable lookup.
  Qt now reproduces the command binding plus preview/limit/cursor/page identities,
  rejects unknown fields and Session/Item/reference or window drift, and emits a
  first request with negotiated ceilings while continuations carry only the exact
  cursor. The Workbench uses the wire Item ID, freezes metadata across pages,
  accumulates raw UTF-8 bytes under Runtime/workflow generations, makes late
  responses inert, and enables explicit Pin only after terminal length, SHA-256,
  and canonical omission-marker uniqueness/head-tail boundary verification. Generic paging for the remaining content domains, generated
  protocol types, and clean Windows execution remain absent; keep `3.8` unchecked.
- The final backend gate passes `1050` Rust tests with one explicitly ignored live
  Codex fixture plus strict workspace Clippy and formatting. Three earlier complete
  runs failed only the large PTY correctness fixture's five-second wait under full
  workspace contention; its focused runs passed, and the final complete run passes
  with a test-only 20-second correctness deadline. Product behavior and dedicated
  performance budgets are unchanged. Strict OpenSpec and diff gates pass before the
  backend commit. A clean isolated desktop configure/build passes. Its first aggregate
  CTest run passed 21 targets while the CTest timer incorrectly expired
  `agent_runtime_protocol` and `aap_generated_types`; exact isolated reruns pass in
  67.32s and 8.55s, giving successful execution evidence for all 23 targets. This is
  recorded as 21 aggregate plus two exact reruns rather than one uninterrupted
  aggregate success.
- The Qt command-output paging gate adds fixed cross-language identities, strict
  first/continuation response validation, actual request-shape evidence, correlated
  valid-response signal routing, malformed response protocol closure, canonical
  truncated/missing/duplicate/moved omission-marker cases, and render coverage for punctuation Item IDs, a UTF-8
  scalar split at 64 KiB, Partial/Load More, terminal Pin, cross-Session rejection,
  and invalidated late responses. The complete desktop build passes. The aggregate
  serial aggregate CTest run passes 22 targets while `agent_workbench_render`
  misses its language-server stop lifecycle state; its exact rerun passes in 7.74
  seconds, so this is recorded as 22 aggregate plus one exact rerun rather than
  23/23. The full
  Rust workspace test passes in 276.66 seconds, strict Clippy and formatting pass,
  generator freshness/negative and the 47-file inventory pass, `aegisy-aap` packages,
  strict OpenSpec passes, and `git diff --check` passes. This is local macOS evidence
  and cannot close the remaining content domains, generated protocol types, clean
  Windows, or live-provider gates.

- Workbench schema v15 adds a dedicated `public_timeline_events` journal and one
  `public_timeline_cursors` source/cursor row per Session. Session insertion registers
  the empty cursor idempotently; event insertion and cursor advancement use the same
  SQLite transaction with compare-and-swap state. A normal WAL-consistent migration
  backup precedes v14-to-v15, and migration backfills an empty registered cursor for
  every existing Session without fabricating historical public events.
- The schema v16 retention foundation extends each cursor with a durable
  sequence/Event-ID/timestamp floor and adds one
  `public-timeline-checkpoint/0.1` projection per Session. The v15-to-v16 migration
  preserves the existing Journal and head exactly while initializing a zero floor
  and no fabricated nonzero checkpoint. The canonical inner
  `event-sequencer-checkpoint/0.1` records the Session anchor plus Turn/Item
  lifecycle only, never Item content/data; it is bounded to 16 MiB, 100,000 Turns,
  and 100,000 Items and uses the domain-separated
  `event-sequencer-checkpoint:sha256:<64 lowercase hex>` identity.
- Internal checkpoint/prune coverage binds checkpoint replacement, floor movement,
  and prefix deletion to one SQLite transaction, restores the Sequencer from the
  checkpoint plus retained tail on restart, rejects identity/anchor/lifecycle/tail
  tampering, and resets Journal/checkpoint/floor atomically during Session purge.
  Public Timeline foreign keys use `ON DELETE RESTRICT`; a regression fixture proves
  a Session delete cannot cascade away Journal authority. A separate startup fixture
  removes a Session projection with foreign keys disabled while preserving a nonzero
  checkpoint and retained tail, then proves pre-recovery replay, Session rebuild,
  final strict ownership verification, anchor sync, and continued Sequencer numbering.
  The prune entry point remains intentionally unreachable by automatic maintenance
  even though public current-Session snapshot recovery is now implemented; enabling
  production pruning remains a separate reviewed stage.
- Stored envelopes retain exact JSON bytes, SHA-256, Event ID, timestamp, Turn, and
  Session-local sequence. Startup verifies the registered cursor/event count and
  anchor, redundant columns, envelope hash/bytes, Event ID, contiguous order, and
  the complete cross-page Turn/Item lifecycle by replaying through a local
  `EventSequencer`. Verification is bounded to 10,000 Sessions and 100,000 total
  public events; malformed, missing, hash-consistent semantically invalid, or
  over-limit authority fails closed.
- Fixed-watermark pages bind both sequence and Event ID, cap one page at 200 events,
  reserve 512 KiB beneath the 4 MiB AAP frame for the page/JSON-RPC envelope, and
  stop before the accumulated event-byte budget without skipping a sequence. A
  single event that cannot fit the replay budget is rejected before either event or
  cursor mutation. Tests wrap a large page in a worst-case 128-byte JSON-RPC request
  ID and keep the serialized response within the exact frame limit.
- Session purge removes all public Timeline envelopes and resets its retained
  tombstone cursor to the registered empty state in the same deletion transaction,
  so deleted conversation content cannot remain replay-readable. Projection rebuild
  may reinsert a Session without resetting an existing public cursor.
- `EventSequencer::prepare` now produces an owned candidate without mutating live
  Session, Turn, Item, timestamp, or sequence state; `commit` rechecks the exact
  sequence/Event-ID baseline and installs that candidate only after notification
  serialization and durable Journal append. A stale ticket is rejected.
  Paginated startup reconstruction independently replays every exact envelope and
  requires both the expected final sequence and Event-ID watermark before installing
  restored state. Focused sequencer tests cover abandoned preparations, preparation
  failures, stale prepared tickets, pagination, forged watermarks, and lifecycle
  replay.
- Normal durable Codex Turn creation, initial user Item, completed Agent/metadata/
  steering Items, command Item plus Artifact Blob, and completed/failed/interrupted
  Turn Trace terminal producers now append their public event in the same SQLite
  transaction as the affected projection rows and internal projection event. Store
  admission requires a public Item to equal the exact sanitized persisted Item,
  including binding, kind/role/state, content/data, and timestamp. Failure injection
  proves a Journal insert failure rolls back Item/projection rows and cursor state;
  raw or timestamp-drifted public Items are rejected without side effects.
- The stable AAP capability `timeline.replay.fixed-watermark` and method
  `timeline/sync` are advertised only with a healthy durable Store. Typed Rust,
  Draft 2020-12 Schema, fixture, Runtime dispatch, and Qt client agree on closed
  request/page objects, sequence-plus-Event-ID anchors, nullable first watermark,
  unchanged continuation watermark, 1-200 limits, contiguous same-Session events,
  response-size bounds, final-anchor semantics, and complete-page validation.
  Forged after Event IDs fail with `-32147`; missing capability or durable storage
  fails closed instead of falling back to projected history.
- A true pre-floor request now maps to the closed `timeline-retention-gap/0.1`
  JSON-RPC `-32148` response. Typed Rust and Draft 2020-12 Schema require exact
  requested after/watermark, durable floor/head, snapshot-required metadata,
  boolean `snapshot_available`, incomplete event history, and
  `replay_from_floor_allowed:false`; the response contains no checkpoint or Item
  content. Runtime returns availability true only for a connection that negotiated
  `timeline.snapshot.current`, while a snapshot-unaware client receives false. A
  retained after or watermark with a substituted Event ID remains `-32147`.
- Schema v17 persists a sanitized visible-state snapshot at the exact floor in the
  same transaction as the content-free Sequencer checkpoint, floor movement, and
  exact prefix deletion. Runtime reconstructs one fixed head from that floor plus the
  retained tail. Restart, transaction-failure, semantic-tamper, ownership, active/open
  Item, and bound fixtures preserve the prior authority without truncation or
  automatic pruning.
- Protocol and Runtime evidence covers the strict request/page Schema and Rust contract for
  null-only first-page capture; exact continuation snapshot identity/watermark/cursor;
  domain-separated complete snapshot, page, and Item identities; contiguous ordinals;
  first/latest Item anchors; active-Turn started/latest anchors and ordered open Item
  IDs; 1-200 Item pages below 4 MiB; and complete-state bounds of 10,000 Items/64 MiB.
  Runtime fixtures cover empty, terminal, and active running-Turn materialization.
  The capability is advertised only with a healthy Store and must be negotiated.
- Qt keeps all pages, totals, identities, active-Turn state, and cursors in private
  per-Session staging. Focused fixtures prove that incomplete, malformed, substituted,
  header-drifted, and invalid-state pages leave the prior projection visible; a
  complete page atomically replaces only the affected Session; delayed events through
  the watermark are dropped; later events pass through ordinary validation; and a
  background Session never contaminates the visible Timeline. Disconnect drops
  incomplete page staging and its accounting while preserving confirmed UI, bounded
  queued live events, their accounting, and snapshot-recovery intent. A negotiated
  `snapshot_available:false` fixture proves that Qt issues no snapshot request and
  retains the confirmed projection and queued event across a later disconnect.
- The Qt AAP client retains each pending `timeline/sync` request until response
  validation and accepts `-32148` only when its exact Schema, Session, after,
  watermark, floor/head window, and snapshot route bind to that request. The
  environment fixture accepts negotiated `snapshot_available:true` and
  unnegotiated `false` responses without disconnecting, while rejecting a
  cross-Session substitution and an unnegotiated `true` claim as protocol violations
  without signalling recovery.
- Qt now keeps recovery state per Session. A detected gap freezes only that Session,
  requests from its last confirmed sequence/Event-ID anchor, stages all validated
  pages and presentation effects privately, and queues bounded live events. No
  incomplete page is visible. Reaching the fixed watermark publishes the staged
  projection once, renders those events, restores active-Turn state, and drains the
  queued live events through ordinary validation. Fixtures cover independent Session
  progress, multi-page atomic visibility, malformed-page rollback, same-sequence
  Event-ID drift, queue overflow, request failure, and missing capability.
- Focused failure fixtures also cover cursor-update rollback, new-Session
  registration, v14 migration backup/backfill, restart paging, live appends between
  fixed-watermark pages, forged watermark Event IDs, payload and redundant-column
  tampering, removed streams, invalid terminal-after-Item history, response-byte
  pagination, oversized event rejection, deletion purge, stale prepared tickets,
  wall-clock rollback, terminal-Item boundary enforcement, aggregate Qt pending
  limits, the 10,001st Session fail-closed gate, initial Session sync, and real
  disconnect signal ordering. The snapshot stage on 2026-07-25 passes 27 AAP type
  tests, 690 sidecar tests plus one ignored live fixture, 13 handshake Runtime and
  17 Schema tests; strict workspace Clippy and formatting; the focused Qt snapshot
  run; all 16 desktop CTests; strict OpenSpec validation; and final diff checks.
- Preview Turns now prepare all six synthetic events through a cloned Sequencer and
  commit the Turn, sanitized user/agent Items, internal projection events, terminal
  state, and all six Public Journal rows in one SQLite transaction. The live
  Sequencer and in-memory Items advance only after commit. Failure injection at the
  fourth Journal insert, internal Item event, terminal update, and sequence
  validation boundaries proves complete rollback and successful exact retry.
- Adapter, transport, protocol, and persistence compensation now atomically commit
  the exact Error Item, Turn Trace, terminal projection, internal trace/terminal
  events, and public failed terminal before notification. Store failure injection
  proves no partial Item, Trace, terminal, internal event, cursor, or Journal growth;
  stdio fixtures prove the unique durable Error Item equals the replayed terminal
  Item and timestamp. An existing Trace cannot receive a later Public Journal repair,
  and the Trace-only Store helper is unavailable in production builds.
  Some streaming/control-only events correctly have no durable Item projection and
  append only to the Journal. The schema v16 floor/checkpoint remains internal
  retention and restart authority, while schema v17 plus fixed-head paging and Qt
  atomic replacement now provide structured retention-gap snapshot recovery. At this
  historical fixed-watermark producer gate, live subscription and complete reconnect
  orchestration were absent; the later live-subscription section supersedes those two
  gaps. Explicit acknowledgement, automatic pruning enablement, and Windows recovery/
  runtime evidence remain absent. Keep `3.5` unchecked.

Model catalog foundation evidence:

- An internal `model-catalog/0.1` contract now validates bounded model identity,
  provider/protocol/role metadata, explicit availability/entitlement/lifecycle
  states, optional limits and capabilities, runtime degradation metadata, and
  field authority labels (`upstream-authoritative`, `aegisy-configured`,
  `evaluation-derived`, `estimated`, or `unknown`). Invalid limits, oversized
  lists, control characters, and credential-shaped metadata fail closed.
- Read-only AAP capability `model.catalog.read-only` and method `model/catalog`
  expose an offline runtime-binding projection. Unknown capability and limit
  values remain JSON `null`; the response reports `state: offline`,
  `signature_validated: false`, `refresh_supported: false`, and
  `contains_credentials: false`. Qt's `AgentRuntimeClient` requests this
  projection after initialization and emits `modelCatalogRead` for future
  picker/projection UI.
- Read-only capability `model.capability-check.read-only` and method
  `model/capability-check` provide a preflight matcher for Chat/Work,
  attachments, tools, reasoning, context-token floors, exact Runtime identity
  and version, model availability/entitlement, and zero-data-retention policy.
  Work mode forces a tool requirement. The result distinguishes `compatible`,
  `blocked`, and `unknown`, returns bounded mismatch codes, and sets
  `selection_allowed` to false unless the catalog is fresh, signed, and every
  required value has a verified authority; present values marked `unknown` or
  `estimated` remain unknown checks.
- Additive `model-runtime-compatibility/0.1` entries preserve the legacy Runtime
  summary while representing Codex App Server, ACP, native, and unknown adapter
  families with canonical adapter/protocol IDs, bounded exact-version sets,
  compatibility state, field authority, evidence version, and structured
  warning/blocking degradations. Validation rejects duplicate adapters, versions,
  degradation codes/features, secret-shaped data, and state/evidence/severity
  contradictions. Capability preflight leaves a Runtime without a version
  `unknown`, blocks a requested version outside an authoritative exact set with
  `runtime-version-not-verified`, blocks missing/incompatible verified adapters,
  and exposes warning degradations without converting them into blockers.
  Protocol evidence proves the offline Preview projection is native / `aap-native`
  / `0.1.0`, retains unknown authority, and exposes no selection, routing, token,
  Turn, or execution authority. A signed production catalog, real ACP/native
  contract fixtures, cloud publication, and macOS/Windows compatibility evidence
  remain absent, so task `9.5` stays unchecked.
- Unit and protocol fixtures cover unknown-value serialization, invalid catalog
  rejection, capability negotiation, capability preflight, and the absence of
  credential-shaped output. Catalog policy fixtures additionally reject
  signature/state conflicts, duplicate aliases and roles, aliases equal to a
  model ID, unsupported authority keys, non-positive limits, and secret-shaped
  catalog/source metadata. Authority classification remains metadata-only and
  does not establish a trusted upstream source. This is only a metadata
  foundation. Signed cloud refresh/cache, durable profiles, and model switching
  remain unchecked under OpenSpec `9.1` through `10.12`.
- Internal `model-catalog-signature/0.1` creates a canonical payload binding the
  signature schema, Key ID, and exact unvalidated catalog. Strict Ed25519
  verification is performed with pinned `ed25519-dalek` 2.1.1; only a successful
  verification can return a catalog with `signature_validated:true`. The bounded
  `model-catalog-key-ring/0.1` validates key encoding, validity, revocation,
  content identity, positive generation, and replacement lineage. Rotation
  preserves prior Key IDs/public keys, advances by one generation, cannot widen
  old validity or undo revocation, and rejects rollback/conflicting generations.
  Four signature/key-ring fixtures cover success, payload tampering, wrong or
  unknown keys, expiry, rollback, missing lineage, revocation, and Key ID
  rewriting. A fifth cache fixture proves forged catalog envelopes fail before
  cache admission.
- Internal `model-catalog-key-ring-signature/0.1` binds the signer Key ID,
  signature time, exact validated Key Ring, payload identity, and Ed25519
  signature. `model-catalog-trust-anchor/0.1` contains only the expected public
  root supplied by the signed host/package boundary; the repository contains no
  invented production root or private key. A first Key Ring must be signed by
  that root, use generation one, and preserve the exact non-revoked root without
  replacement lineage. Every later Ring must be signed by a known, currently
  active and unrevoked key from the previous generation before structural
  rotation validation runs; a new key cannot authorize its own admission.
- Private `model-catalog-trust-store/0.1` persists that anchor and current Key
  Ring outside project roots in a bounded atomically replaced snapshot with
  private Unix permissions and content identity validation. First open persists
  an empty anchor snapshot, so later snapshot deletion fails closed instead of
  re-enabling generation-one bootstrap. Wrong anchors, forged signatures,
  unknown/expired/revoked signers, generation gaps, snapshot tampering, and
  missing snapshots fail closed; disk commit failure restores the previous
  in-memory Ring. Six Trust Store fixtures cover bootstrap, signed rotation,
  restart, catalog/cache verification, signer denial, tampering, deletion, and
  rollback. Cache Store public admission now requires this Trust Store, while
  its lower-level catalog mutation is module-private.
- The Trust Store is not yet opened by Runtime/AAP/Qt, is not Workbench-SQLite
  event-backed, and has no real Aegisy production root. There is no authenticated
  Key Ring/catalog endpoint, conditional request flow, private signing service,
  key-publication service, cloud refresh, or desktop transition evidence. Tasks
  `9.3`, `9.4`, and `10.1` therefore remain unchecked.
- The internal `model-catalog-refresh/0.1` contract now validates only a
  host-owned authenticated transport observation. It binds bounded conditional
  ETag/Last-Modified validators, requires `Accept-Encoding: identity`, accepts
  only JSON 200 responses with a bounded signed bundle or a validator-backed 304,
  and classifies authentication, redirect, rate-limit, server, content-encoding,
  size, and malformed-body failures without retaining response content. The
  deterministic 304 fixture and nine unit tests prove validator preservation,
  content-free errors, and false authority flags. Read-only AAP
  `model/catalog-refresh-status` and its Qt projection expose only the current
  `unconfigured` state; no network request, credential transfer, Trust Store
  install, cache mutation, model selection, token, routing, or Turn authority
  exists. This is partial evidence for `9.4`/`10.1`, not completion.
- Internal `model-catalog-cache/0.1` accepts only a clean catalog marked fresh
  and signature-validated with a bounded future expiry. It content-hashes the
  catalog, binds a positive monotonic sequence and receipt time, treats exact
  same-generation replay as idempotent, rejects older or conflicting
  generations, and derives explicit fresh/stale/expired views with a bounded
  stale window. Clock regression and snapshot identity tampering fail closed;
  expired entries return no catalog metadata. Every view fixes
  `selection_allowed:false`; the Store's public admission path now requires the
  root-anchored Trust Store and signed catalog verifier, while the low-level cache
  mutation remains private to the module. Seven focused unit fixtures pass. A private
  `model-catalog-cache-store/0.1` now persists the cache outside project roots
  through an atomically replaced, bounded snapshot with private Unix
  permissions, restart identity validation, and tamper detection. Runtime opens
  it from the durable data root and uses an in-memory fallback only without
  durable storage. Runtime exposes `model.catalog.cache.read-only` and
  `model/catalog-cache`; an empty cache returns no catalog body and an explicit
  false selection authority, while Qt validates and displays that state in the
  model-binding tooltip. The Store is not Workbench-SQLite event-backed, and
  Runtime still does not open the Trust Store and has no production anchor,
  authenticated cloud refresh, non-empty host transitions, or selection
  authority, so tasks `9.3` and `10.1` remain unchecked.
- The Qt Workbench now validates and projects the catalog/cache state boundary
  separately: offline, invalid, fresh, stale, expired, and empty states are
  shown as bounded model-binding status text. A malformed response or any
  `selection_allowed=true` cache response becomes an explicit invalid state;
  it is never silently cleared or treated as a usable catalog. The render
  fixture covers offline plus fresh/stale/expired/empty lifecycle states and
  malformed catalog/cache responses. The complete desktop CTest suite now executes
  these assertions successfully on this host; product-authority and cross-platform
  gates still prevent the milestone from closing.
- The Qt host also consumes the read-only `model/capability-check` projection
  for the current catalog-bound model. Chat and Work requirements are rebuilt
  on mode changes; result validation binds the exact model identity, bounds
  check/mismatch arrays, and requires decision/`selection_allowed` consistency.
  Compatible, blocked, unknown, and malformed results render as explicit
  metadata-only status, with compatible still labeled read-only. No picker,
  routing, credential, Turn, or execution authority is granted. The render
  fixture executes all four outcomes in the passing desktop CTest suite.
- The 2026-07-21 catalog, matcher, profile, catalog-policy, cache, and profile-store
  foundation stage passed 428 Rust unit tests with one ignored live fixture, 62 protocol tests,
  11 stdio/Codex tests, strict Clippy, the complete CMake build, and CTest
  `agent_runtime_protocol`.
- The later 2026-07-21 root-anchored Trust Store stage passed 434 Rust unit tests
  with one ignored live fixture, 62 protocol tests, 11 stdio/Codex tests, strict
  Clippy, the complete CMake build, and CTest `agent_runtime_protocol`.
- The later 2026-07-21 Runtime compatibility metadata stage passed 439 Rust unit
  tests with one ignored live fixture, 62 protocol tests, 11 stdio/Codex tests,
  strict Clippy, formatting, the complete CMake build, CTest
  `agent_runtime_protocol`, and strict OpenSpec validation. Task `9.5` remains
  unchecked for the production catalog, real adapter fixtures, authenticated
  publication, and cross-platform evidence listed above.

Model profile foundation evidence:

- Internal `model-profile/0.1` validates global and project scope, bounded
  `agent`, `plan`, `apply`, `review`, `utility`, `embedding`, and `rerank` role
  bindings, content-free source metadata, and a deterministic SHA-256 identity.
  `single_model` creates the conservative default with only the Agent role
  enabled; `resolve_role` never silently falls back to the default model for an
  unconfigured role. Role-specific bindings and disabled explicit bindings are
  represented separately, and secret-shaped metadata is rejected.
- Six unit fixtures cover the default one-model shape, project-scope identity,
  single-model mismatch, explicit role-specific resolution, disabled roles, and
  secret-free content identity. Only the metadata-only AAP/Qt projection is
  exposed; no writable control, catalog picker, token, routing, or execution
  authority exists. OpenSpec tasks `10.3` and `10.4` remain unchecked.
- Internal `model-profile-store/0.1` stores one global and bounded project
  profiles in a private, atomically replaced snapshot. It validates profile and
  project scope, rejects duplicate profile IDs and secret-shaped metadata,
  requires exact revision CAS for updates/removals, makes identical retries
  idempotent, and rechecks the content-hashed snapshot after restart. Seven
  focused fixtures cover create/reopen, revision conflicts, removal, duplicate
  IDs, secret rejection, tamper detection, invalid project IDs, and private
  bounded storage. This snapshot store is not itself Workbench-SQLite event-backed
  or writable through AAP/Qt, and is not bound to catalog capability checks or
  routing; it remains the current Runtime read authority. `10.3` and `10.4`
  therefore remain unchecked.
- SQLite schema v14 adds a separate `model_profiles` projection with one global
  scope and bounded historical project scopes. Save/update/remove uses exact
  revision CAS, monotonic generation and event sequence, idempotent retries, and
  one `IMMEDIATE` transaction for the lifecycle event plus projection. Startup
  revalidates canonical profile JSON/hash/identity, the complete bounded event
  chain and cursor, current-lifecycle creation time, project ownership, orphan
  streams, and fixed false selection/routing/token/Turn authority. Capacity is
  enforced before commit for 256 active profiles, 1,025 historical scopes, and
  10,000 lifecycle events; active profile IDs remain globally unique across scopes.
  Fixtures cover event rollback, silently ignored projection writes, event-capacity
  rejection, duplicate active IDs, secret-shaped metadata, semantic/timestamp/cursor
  tampering, orphan cursors, dedicated hashed streams, v13 backup migration, restart
  durability, exclusion of model-profile streams from Session recovery, and
  side-effect-free rejection of the reserved stream namespace by ordinary Session
  creation, portable import, and projection rebuild. This
  stage does not migrate or delete the snapshot,
  connect Runtime/AAP/Qt to the SQLite projection, select a model, route a request,
  issue a token, or start a Turn; tasks `5.1`, `5.2`, `10.3`, and `10.4` remain
  unchecked. The complete stage passes 616 library tests with one ignored live
  fixture, 10 context-threshold tests, 63 protocol tests, and 19 stdio tests, plus
  formatting, strict Clippy, strict OpenSpec validation, and `git diff --check`.
- Runtime now opens the validated Profile Store when durable storage is healthy
  and negotiates `model.profile.read-only`. AAP `model/profile/list` returns a
  bounded snapshot generation/identity and metadata-only profile views, with an
  optional project filter; `model/profile/read` resolves a profile ID and
  returns a stable not-found error when absent. List/read responses and each
  view explicitly set `selection_allowed:false`, `routing_authority:false`,
  `token_issued:false`, and `turn_started:false`. The protocol fixture covers
  capability negotiation, an empty legal snapshot, all false authority fields,
  and missing-profile error stability. This remains an inspection projection:
  it does not write profiles, select models, issue tokens, start turns, connect
  to the catalog matcher, or provide Qt picker/switching controls, so tasks
  `10.3` and `10.4` remain unchecked.
- Qt `AgentRuntimeClient` now negotiates `model.profile.read-only`, requests
  `model/profile/list`, validates the list schema and all four false authority
  flags, and emits a typed result signal. `AgentWorkbenchWidget` displays only
  a bounded "Profile metadata read-only" count in the existing model-binding
  tooltip; it does not populate selectable models or initiate profile reads,
  routing, token issuance, or turns. CMake compilation covers the Client and
  render targets. The focused Qt runtime/render CTest processes were killed by
  host resource pressure before assertions, so their runtime evidence remains
  pending and no UI milestone is claimed.

Autonomy release-gate evidence:

- OpenSpec `21.1` is complete. `runtime/degradations` reports bounded,
  content-free disabled gates for background jobs, multi-agent execution, and
  unattended writes. Each gate is explicitly `not-advertised`, has
  `stable_enabled=false` and `override_available=false`, and names the missing
  prerequisite tasks. The initialize response advertises no autonomy capability,
  and protocol coverage rejects a hidden background dispatch method. No
  scheduling, child execution, Agent write, or unattended mode is enabled by
  this gate.

Structured-plan foundation evidence:

- An internal `structured-plan/0.1` contract validates bounded step IDs,
  statuses, owners, dependency references/cycles, content-free SHA-256
  evidence, and required completion evidence. Base/evidence revision changes
  mark affected steps stale without silently changing their status. Four unit
  fixtures cover valid graphs, cycle/unknown dependency rejection, revision
  drift, and secret/non-hash rejection. AAP/UI persistence, plan questions,
  durable job state, and execution remain unavailable under task `21.2`.

Child-task contract evidence:

- An internal `child-task/0.1` request contract binds parent session/turn,
  bounded goal and context identities, project/root isolation, tools, model and
  permission policy, six independent budgets, and a bounded
  `child-handoff/0.1` result shape. Validation grants no permissions or
  execution authority. Four unit fixtures cover identity, unsafe scope,
  duplicate/budget bounds, and secret/result rejection; lineage, approvals,
  scheduling, worktree allocation, and executor integration remain open under
  task `21.3`.

Child-task lifecycle evidence:

- An internal `child-task-state/0.1` state machine validates parent/child
  identity, generation-bound transactional status transitions, cancellation
  request/rejection/acknowledgement, completion races, generation exhaustion,
  and content-free bounded handoff references/counts. Six unit fixtures cover
  binding, invalid transitions without partial state, idempotent and rejected
  cancellation, cancel-vs-complete races, exhaustion, and handoff rejection.
  Durable child-session lineage, navigation, AAP status/cancel methods, and
  parent review are still unavailable under task `21.4`.

Child worktree admission evidence:

- Internal `child-worktree-admission/0.1` validates the exact child-task and
  runnable lifecycle identities against the existing live Git worktree owner,
  base revision, registration, lock, path, health, and clean admission state.
  Five real-Git fixtures prove shared read-only behavior, healthy write-capable
  admission, mandatory dedicated isolation, cross-child reuse denial, dirty
  denial, and cancellation denial. The returned runtime proof is
  non-serializable, while its content-free receipt explicitly grants neither
  permission nor execution. Durable scheduling, project-root registry binding,
  per-tool revalidation, production permission/approval/sandbox intersection,
  executor integration, AAP/Qt controls, and Windows evidence remain open under
  task `21.5`.

Child runtime budget evidence:

- Internal `child-runtime-budget/0.1` reserves non-zero token/cost ceilings before
  model admission and transactionally tracks wall time, turns, tools, active
  concurrency, and policy-bound network requests. Authoritative and estimated
  usage retain provenance; unknown token or cost usage consumes the full
  reservation rather than becoming zero. Content-free snapshots return limits,
  used/reserved/remaining values and warning/saturated/exhausted dimensions but
  no operation IDs, content, permission, or execution authority. Snapshot validation
  also rejects out-of-contract limits, impossible reservation/source accounting,
  inconsistent remaining values, forged classifications, and authority flags. Seven unit fixtures
  cover warnings, conservative unknown settlement, overcommit with no partial
  state, concurrency saturation/recovery, independent tool/network limits,
  network-policy mismatch, wall exhaustion, clock regression, generation
  exhaustion, and snapshot tampering. Provider usage, Runtime monotonic-clock integration, durable
  scheduler/session events, cancellation/refund rules, executor admission, AAP/Qt
  events, and endurance/cross-platform evidence remain open under task `21.6`.

Unified execution pipeline evidence:

- Internal `unified-execution-plan/0.1` emits the same ordered 14 stages for
  interactive, child, and background envelopes. Requirements are derived from
  mode and mutation policy rather than caller-selected omissions: child work
  requires budget/release and write isolation/approval when mutating; background
  additionally requires a durable job, notification, and unattended approval.
  Invalid child/job/unattended identity relabelling fails. The existing Codex
  read-only interactive `turn/start` invokes the planner immediately before real
  adapter dispatch, and all eleven stdio adapter tests pass through that path.
  Six unit fixtures prove exact cross-mode stage order, current interactive
  readiness, child/background missing-gate projection, binding rejection, and
  common reconciliation failure. Plans expose no permission or execution ticket.
  Typed proof composition, child/background dispatch, generic executor ownership,
  durable job/session events and recovery, budget/provider settlement, AAP/Qt mode
  status, and cross-platform evidence remain open under task `21.7`.

Background job lifecycle evidence:

- Internal `background-job-request/0.1` and `background-job-state/0.1` validate a
  content-free, persistence-ready job record with project/session/root, unified
  execution plan, optional child, idempotency, manual/one-shot schedule, bounded
  attempts/backoff, and optional safe retry boundary. Transactional transitions
  separate pause/cancel requests from acknowledgement, preserve waiting approval,
  bind every terminal attempt to evidence, and allow completion to win a cancel
  race. Restart marks active work interrupted rather than successful; only a
  pre-bound safe boundary can make it retry-eligible, and decisions never auto
  retry or approve. Schedule/retry backoff and cancellation cannot be bypassed by
  pause/resume/start. Six fixtures cover completion, pause/approval, cancellation
  race, idempotent bounded retry, queued/running restart, invalid schedule, and
  generation fail-closed behavior. Scheduler/process ownership, authoritative
  approval, notification, AAP/Qt control, and endurance/cross-platform evidence
  remain open under task `21.8`.
- Workbench schema v11 retains the bounded `background_jobs` projection whose canonical
  request/state JSON, SHA-256, request/state identity, generation, status,
  cancellation, attempt count, schedule, and timestamps are revalidated on every
  read and during a 10,000-row startup scan. Create and generation-CAS update commit
  with typed `background-job.*` events in one database transaction; identical
  retries are idempotent, stale state fails, and an injected event failure leaves no
  projection row. Active records block deletion/retention and terminal records are
  purged with their session. Four store fixtures cover reopen/event replay, rollback,
  tamper-triggered read-only startup recovery, deletion protection, and WAL-consistent
  v9-to-v11 backup migration.
- Schema v11 adds a bounded `background_job_leases` projection with exact
  job/request/state generation, scheduler owner, lease generation, bounded
  acquire/renew/expiry times, optional verified process registration/process hashes,
  canonical JSON/hash, and fixed false dispatch/takeover authority. Generation-CAS
  acquire, renew, state rebind, process bind, release, and expiry commit with typed
  `background-job.lease-*` events. Event failure rolls back, startup tampering enters
  read-only recovery, and v10-to-v11 uses the WAL-consistent backup. Active leases
  protect deletion after terminal job state; a stale lease can expire without
  adopting the newer generation. Four contract fixtures cover bounded acquisition/
  renewal, expiry/release, state drift/tampering, and verified process binding. Five
  store/migration fixtures cover CAS/idempotency, event rollback/tampering, explicit
  state rebind and terminal deletion protection, stale expiry, and v10 migration.
- Internal `background-job-scheduler/0.2` loads only a complete 1,000-record-or-less
  recovery set and atomically replaces one owner-identity/generation-bound snapshot.
  Schedule/admission, pause, approval wait, retry review, terminal review,
  monitor-owned-process, and manual reconciliation are explicit. Lease states expose
  missing, current, expired, released, stale-job, and owner-mismatch evidence;
  process ownership separately exposes missing lease/registration, unavailable or
  non-running observation, mismatch, and exact current ownership. Internal
  `background-job-process-observation/0.1` accepts only an owned `Child` handle and
  binds exact owner/job/request/state/generation/attempt/time evidence without
  returning a PID, command, path, environment, or output. Owned-running, owned-exited,
  absent, inaccessible, mismatched, and unknown are distinct. Exact running
  ownership becomes monitor-only only when a current durable lease carries matching
  verified process-registration and process identities; every other active result is manual, and process exit
  requires a terminal job event without implying completion. Pending cancellation
  separately requires acknowledgement. Every entry denies dispatch and automatic
  retry/approval/takeover. Invalid owner, time, limit, lease, observation, store
  failure, or truncation preserves the previous snapshot. Eleven scheduler/process fixtures cover
  due/future queue review, active/cancel handling, approval/retry behavior, absent
  ownership, lease expiry/staleness, terminal release review, exact-generation
  rebinding, real macOS running/exited observation, and transactional refresh. A
  cfg-gated Windows fixture is present, but target
  compilation currently stops in bundled SQLite C before this module because this
  host lacks Windows SDK headers; a Windows runner is still required. Automatic
  lease acquisition/renewal, restart process adoption, recovery mutation,
  dispatch, notifications, AAP, and Qt remain absent.
- Internal `background-job-recovery-decision/0.1` converts one fully validated
  scheduler snapshot entry into a content-free audit record. It binds exact job
  status/cancellation and generation, scheduler owner/generation/snapshot/entry,
  lease and optional process-observation evidence, bounded blocker codes/hash/count,
  and timing.
  Store append rechecks current job and lease evidence before and under the write
  transaction, is idempotent for the same snapshot entry, and emits only
  `background-job.recovery-reviewed`. The journal is capped at 10,000 events and
  semantically revalidated on startup. Three fixtures prove durable restart replay
  and idempotency, forged-snapshot/stale-job rejection, event/sequence rollback,
  and startup rejection of hash-consistent semantic tampering. Every decision keeps
  automatic retry/approval/takeover, dispatch, and mutation authority false and
  never changes job, lease, or process state. Automatic production/consumption,
  recovery actions, notifications, AAP, and Qt remain absent.
- Internal `background-job-notification-intent/0.1` derives four content-free intent
  kinds from an exact validated job request/state and optional semantically validated
  child-budget snapshot. It binds request/state/job/scope/evidence identities,
  exhausted dimensions, and a stable deduplication identity while separately hashing
  creation time into the full intent. Budget validation rechecks remaining values,
  counters, usage-source totals, classifications, scope, and false authority flags;
  a valid generation-zero wall-time exhaustion snapshot is not rejected. Six fixtures
  cover completion, failure, approval wait, budget exhaustion, unsupported state,
  identity and delivery-authority tampering, forged accounting, task drift, and stable
  deduplication. Intent fields fix content inclusion, delivery availability/attempt,
  and platform authority to false.
- Workbench schema v12 persists one canonical intent per deduplication identity in
  `background_notification_outbox` and appends the exact
  `background-job.notification-recorded/0.1` session event in the same `IMMEDIATE`
  transaction. Projection rows bind the job/request/state/generation, canonical
  JSON/hash, event sequence, creation/record time, `recorded` state, zero attempts,
  and false content/delivery/platform authority. Identical retries return the original
  record even after the job advances; stale first writes and dedup conflicts fail.
  A bounded session-scoped keyset page is ordered by record time descending and intent
  identity ascending, and validates its anchor before use. Startup semantically checks
  at most 10,000 outbox records against both exact job lifecycle evidence and their
  session events. The v11-to-v12 migration creates a WAL-consistent backup, preserves
  existing sessions, and creates the empty outbox/indexes. Terminal session purge
  removes jobs, outbox rows, and notification events together.
- Six intent fixtures plus five store/migration/deletion fixtures cover all four kinds,
  accounting/identity/authority tampering, dedup stability, durable restart, paging,
  cursor forgery, progressed-state idempotency, event/projection rollback, canonical
  projection and lifecycle tampering, migration, and purge. The complete batch passes
  392 Rust unit tests with one ignored live fixture, 56 protocol tests, 11 stdio/Codex
  tests, strict Clippy, formatting, `git diff --check`, the complete desktop build, and
  CTest `agent_runtime_protocol`. Strict OpenSpec validation was attempted but the
  Node process was killed by the host with exit 137 and emitted no schema diagnostic.
  Scheduler production, AAP/Qt inspection/settings, delivery state transitions,
  platform permission/delivery APIs, localization/privacy review, and macOS/Windows
  evidence remain absent, so task `21.9` stays unchecked.
- Capability `background-notification.outbox.read-only` is advertised only when the
  durable Workbench store is writable. AAP `session/background-notifications` accepts
  a session, optional structured keyset cursor, and 1-100 limit; it remains readable
  across archive, pending deletion, reconciliation block, and session quarantine, but
  is unavailable in whole-store recovery. Empty pages still bind the session. Missing
  storage, forged cursors, cross-session anchors, and invalid limits fail explicitly.
  Two protocol fixtures cover unavailable storage plus two-kind paging, false authority,
  forbidden content-field absence, cursor forgery, and restart replay. The complete
  Rust baseline now passes 392 unit tests with one ignored live fixture, 56 protocol
  tests, 11 stdio/Codex tests, and strict Clippy.
- Qt `AgentRuntimeClient` exposes only the structured read request/result signal. The
  Session context menu enables a metadata-only viewer from negotiated capability;
  the dialog validates schema, session, cursor, notification/intent versions, allowed
  kinds/statuses, event/generation/time identities, zero attempts, every false-authority
  field, and forbidden content fields before rendering. It supports keyset pagination,
  empty and request-failure states, and has no delivery action. The complete Qt target
  builds. Its render fixture exercises the real empty-outbox request and dialog, but
  this host killed that process twice at startup (0.53s and 0.01s) without assertion
  output; a later lightweight Qt environment test was also killed at 0.42s, while
  `agent_runtime_protocol` continued to pass. Rerun Qt CTest when host memory is
  available. This is not evidence of platform notification delivery.

Background recovery inspection evidence:

- Capability `background-job.recovery.inspect` advertises only with writable durable
  Workbench storage. AAP `session/background-recovery` rebuilds a bounded read-only
  scheduler snapshot, filters it to one session, orders entries by job ID, and pages
  with an entry-identity cursor. Forged or stale anchors fail explicitly. The page
  carries only job/request/state identities, status, cancellation, schedule, lease and
  process ownership labels, blockers, and a matching metadata-only journal review;
  content, process paths, PIDs, commands, and result bodies are absent. Dispatch,
  automatic retry/approval/takeover, and mutation authority are fixed false. Missing
  durable storage returns `-32024`; whole-store recovery does not advertise the
  capability. Protocol coverage includes missing-storage and empty-page authority
  checks, while the scheduler and recovery-decision unit suites cover populated entry
  and tamper cases.
- Qt adds `后台恢复状态…` to the Session context menu when the capability is
  negotiated. Its dialog validates page/entry/cursor/review identities, allowed
  status/action/lease/process enums, blockers, timestamps, content exclusion, and all
  false-authority flags before rendering keyset pages. It has an explicit empty state
  and `加载更多` control and exposes no mutation or delivery action. The full Qt build
  passes; the host-killed render process remains an environment limitation rather than
  a passing render assertion.

Current editor evidence:

- The macOS Qt build and all 16 desktop tests pass.
- The Monaco render test opens two real workspace files, binds them to separate
  editor groups, switches native save targeting through group focus, verifies an
  atomic save cannot modify the other group file, and restores both groups in a
  new workbench instance.
- The native Qt render test verifies that the single-editor safety fallback keeps
  split controls disabled when WebEngine/Monaco is unavailable.
- Workspace search tests cover filename/text matching, Git and sensitive-path
  exclusion, page append, cancellation, stale cursors, result-to-editor navigation,
  and UI stale indicators after a user save.
- Repository indexing tests cover official Tree-sitter symbol extraction for Rust,
  Python, JavaScript, TypeScript/TSX, and C/C++; dependency edges; unchanged-file
  reuse; changed/deleted-file refresh; sensitive, symlink, and Git-ignore exclusion;
  symbol ranges/provenance; and token-budget truncation with focus-file ranking.
- The native workbench test opens a real Rust fixture, lazily builds its index,
  renders the symbol and dependency views, and verifies a non-empty repository map.
- Language-server tests cover bounded `Content-Length` framing, Unicode file URI
  mapping, workspace/symlink denial, server discovery, restricted rust-analyzer
  configuration, lifecycle stop, and real clangd definition and diagnostic flows.
  The AAP protocol and native workbench tests exercise an installed clangd with
  UTF-16 cursor mapping, unsaved document diagnostics, result navigation, and
  explicit unavailable behavior when clangd is absent.
- Observed-diagnostic tests cover SHA-256 identities and raw references, bounded
  artifact truncation, source snapshot replacement, fresh-to-stale invalidation,
  AAP raw retrieval, and the Qt provenance/raw/stale workflow. Raw artifacts contain
  only normalized diagnostics that already passed workspace path filtering.
- Turn-context tests cover authoritative file rereads, changed-revision stale
  metadata, ignored/outside-path denial, item/UTF-8/total byte limits, explicit
  truncation markers, structured AAP transport, and the Qt add/include/remove/send
  workflow across file, selection, search, diagnostic, terminal, and Git sources.
- Workspace/index edge tests cover Unicode paths, case sensitivity, symlinks,
  rename/delete/watch behavior, external edits and stale saves, 5,000-file and
  20,000-symbol limits, project-scoped cancellation, late-result suppression, and
  preservation of the previous complete index snapshot.
- macOS PTY tests cover executable shell discovery, scrubbed environment, canonical
  project-root revalidation, Unicode byte I/O, resize, nonzero exit status, failed
  startup, 1 MiB tail capture, foreground process-group interrupt, close, Work-mode
  binding, cross-session denial, and absence of an Agent terminal-open method.
- Named terminal lifecycle tests cover one foreground terminal per Work session,
  unique Unicode-aware background names, list/attach with absolute output offsets,
  cross-session denial, invalid-restart preservation, successful restart generation,
  stop-to-exit polling, exited-terminal removal, and runtime cleanup. Legacy
  read/close methods remain protocol aliases; no Agent terminal input method exists.
- The native Workbench and local xterm.js 6.0.0/FitAddon 0.11.0 render test run a
  real macOS PTY end to end, verify fitted nonblank rendering, streamed UTF-8 bytes,
  native clipboard copy/paste, exit observation, cleanup, CSP/local-only loading,
  and native-output fallback. A WebEngine renderer restart reloads the trusted page
  and reattaches to the runtime-owned bounded output tail by generation and offset.
- Codex command-translation fixtures are generated against the installed App Server
  0.144.5 schema and cover command started/outputDelta/completed ordering, stable
  item identity, parsed actions, cwd/status/process/source fields, duration, exit
  result, Unicode-safe 256 KiB output retention, 32 KiB timeline display, and
  conservative risk classification. The Qt renderer treats command/model output as
  selectable plain text. The Codex parent process now uses the shared allowlisted,
  secret-scrubbed environment builder and exposes its value-free hash; individual
  child-process environment identity remains vendor-unreported, so task 14.5 is not
  complete and no new Agent execution permission is exposed.
- Task `7.10` has deterministic redacted fixtures for initialization, session/provider
  lifecycle, partial streaming, usage/plan/diff metadata, approval denial, cancellation,
  reconnect, compaction degradation, provider failure, unloaded provider state, and
  failed compensation. Protocol tests parse every JSONL message, reject credential-
  shaped content, and assert stable error codes and recovery transitions. Real stdio
  tests cover the corresponding adapter paths. The Qt render fixture injects opaque
  provider failures for restart, archive, unarchive, and fork, then proves only the
  operation, stable code, and recovery guidance reach labels/tooltips. This fixture
  milestone does not expose provider delete/compact or weaken the read-only adapter.
- Task `7.9` now has a pinned `runtime-degradations/0.2` capability-matrix
  foundation. The matrix test reads the vendored Codex `0.144.5` v2 schema and
  proves exact `87/68/18` request/notification/item coverage plus the checked-in
  SHA-256 and deterministic matrix identity. Provider list/read stdio fixtures
  inject private name, preview, cwd, path, provider, session, and item sentinels;
  their `0.2` projections contain none of those values and expose only bounded
  content-free metadata, domain-separated hashes, and validated lossless opaque
  cursors. Cursors above 4 KiB or containing control/credential-shaped content are
  rejected with the complete projection rather than redacted into a different token.
  A real Turn fixture injects an unknown notification and proves that only one
  bounded method hash/count appears in `runtime/health`, while `session/read`
  contains no notification record or body. A separate server-request fixture
  proves `item/tool/requestUserInput` receives fixed `-32601` without a fabricated
  result, answers, questions, or secret content. Qt focused coverage proves
  pending/invalid degradation blocks every new-Turn path but not Stop, unknown or
  cross-bound live events are inert, item identity cannot change kind/role, and a
  malformed replay page leaves the current Timeline unchanged. Runtime event
  counters and Qt cursors are Session-scoped: a protocol fixture proves two
  Sessions independently emit `1..5` before the first resumes at `6`, and a Qt
  fixture proves switching Session cursors does not create a false gap. Chat/Work
  switching is disabled while the single active Turn is running. The full Rust
  stage passes 630 library tests (one ignored), 63 protocol tests, and 21 stdio
  tests; both the focused degradation/Timeline run and ordinary Qt render run pass.
  The later fixed-watermark, snapshot, bounded-reconnect, and live-subscription
  sections above now add durable public replay, bounded per-Session gap catch-up,
  internal checkpoint-plus-tail restart authority, structured retention-gap recovery,
  and live delivery. Durable Turn-start acknowledgement is covered by the dedicated
  section above; complete Windows recovery/runtime evidence remains incomplete. Full
  vendor capability negotiation and the remaining desktop/dependent feature gates
  are also incomplete, so `3.5` and `7.9` remain unchecked.
- Large command-output tests cover a Unicode-safe 64 KiB head/192 KiB tail,
  1 MiB artifact head/tail, exact omission metadata, 100,000 deltas, and
  content-addressed session isolation/eviction. Output is redacted before capture;
  assignment, authorization, known token, JWT, split-delta, unterminated, and long
  secret cases are absent from the inline timeline, artifact, and AAP response.
  Codex stdout uses a 4 MiB bounded frame reader and fixed 16-message synchronous
  queue; adversarial tests prove exact-limit framing, oversized-frame draining, and
  producer backpressure/unblocking. The Qt render test verifies the session-bound
  full-output action; its dialog is read-only plain text and artifacts are never
  attached to model context implicitly. Task 14.6 is complete.
- Cancellation tests pin Codex App Server `0.144.5` `turn/interrupt` fields, keep
  request/acknowledgement/terminal states distinct, bind requests to exact active
  session/turn identity, and cover duplicate, stale, and completed races. A real
  sidecar stdio test holds a fake Codex turn open, saturates the 32-entry normal
  request queue, observes stable overload errors, then proves out-of-band cancel,
  vendor acknowledgement, and `turn.interrupted`. The Qt render test verifies
  stable Stop/Stopping/Send states. A second macOS process fixture uses the same
  blocked-turn and saturated-queue condition to prove out-of-band user-terminal
  stop, exact terminal ownership, PTY exit, and foreground-child cleanup through
  the shared terminal registry. Task 14.7 remains incomplete because native Agent
  command/daemon process producers do not exist to supply their process-tree
  cancellation evidence and Windows Job Object execution still needs a runner.
- Command-diagnostic fixtures cover rustc/cargo, Clang/GCC, MSVC/TypeScript,
  Ruff/mypy/Pyright, ESLint, pytest, Rust panic, ANSI, Unicode-safe bounds, 200-item
  truncation, and unknown-command rejection. Production ingestion revalidates cwd
  paths through project/sensitive/Git-ignore/symlink/text/revision policy and
  sanitizes command metadata before events. A real sidecar/fake-Codex stdio flow
  proves command delta ingestion, `diagnostics.observed`, forced small-output
  artifact authority, project-scoped normalized raw retrieval, and Qt provenance/
  navigation rendering. Task 14.8 is complete; live Windows compiler/process
  coverage remains in 14.9.
- macOS PTY process fixtures preserve raw ANSI and Unicode bytes, cover interactive
  resize/nonzero exit and failed workspace startup, interrupt a real foreground
  command, stop a named terminal while model dispatch is blocked, and exercise the
  fixed HUP/TERM/SIGKILL escalation against a process group that ignores graceful
  signals. The forced path is time-bounded and leaves no live foreground group.
  Task 14.9 remains incomplete pending equivalent clean-Windows ConPTY/Job Object
  execution and future gated native Agent command/daemon producers.
- Workspace-edit schema tests cover versioned create/update/delete/rename JSON,
  canonical root identity, lowercase SHA-256 base/content descriptors, content-
  reference binding, normalized UTF-8 relative paths, operation/path bounds,
  duplicate target denial, round-trip validation, forged root identity, path
  escape, ambiguous separators, and reference tampering. Task 15.1 defines only
  the internal contract; no AAP apply method or Agent write capability exists.
- Workspace-edit preview tests reread raw base bytes, verify SHA-256 identity, and
  cover create/update/delete/rename unified diffs, aggregate/per-file additions and
  deletions, Unicode content, inline/source truncation, content-addressed paging,
  session/project/root denial, store bounds, missing/mismatched/oversized content,
  and sensitive/Git-ignore/stale/symlink/existing-target warnings. Sensitive base
  contents are absent from preview artifacts. A real Qt/sidecar render flow pages a
  diff larger than 64 KiB, renders the Changes table and sensitive-path warning,
  and proves disk contents remain unchanged. The protocol fixture also proves the
  unimplemented apply method returns method-not-found. Task 15.2 is complete while
  public/Agent mutation remains gated behind later permission/approval milestones.
- Codex file-change Proposal coverage validates the pinned `add` full-content,
  `delete` full-preimage, `update` headerless-diff, and pure-rename semantics;
  treats `patchUpdated` only as volatile parsed-so-far preview and uses canonical
  `item/started` as Proposal authority across relative/absolute path, partial add,
  update/delete diff-shape, and pure-rename differences; requires exact canonical
  Started/completed equality plus required monotonic lifecycle timestamps; enforces
  the shared 256 pending/Started Item and 16 MiB retained-state limits;
  rejects root escape, duplicates, malformed/stale bases, NUL/size/format drift,
  completed-write status, and lifecycle/request mismatches; and preserves BOM plus
  LF/CRLF without touching disk. New records use
  `workspace-edit-proposal/0.2`; Store admission, direct read, and startup/restart
  revalidate aggregate/ordered per-file summaries and recompute
  additions/deletions from every untruncated persisted diff Blob. Hand-written
  `workspace-edit-proposal/0.1` fixtures preserve exact canonical serialization,
  Proposal/preview identities, and read back only as `files_complete:false` /
  `legacy-incomplete`, with unavailable summary semantics left null rather than
  fabricated. Workbench schema v18 fixtures prove immutable Proposal identity,
  domain-separated nested identities, exact artifact coverage,
  false mutation/approval/apply authority, same-edit idempotency/conflict, atomic
  Proposal/Blob-reference/event rollback, restart read, tamper rejection, and
  WAL-consistent v17 migration backup. Failure injection after SQLite commit proves
  committed Proposal/Blob references are never compensated away, while Proposal or
  referenced-Blob corruption quarantines only the owning Session and leaves healthy
  Sessions readable. The real stdio approval fixture opens a Work
  project, receives `patchUpdated`, Started, approval, resolved, and declined
  completion in order, proves Proposal persistence before the adapter can continue
  its decline path, reopens the Proposal after shutdown, and verifies the proposed
  file was never created. The fixture additionally binds the exact
  `codex-app-server` / `codex-cli 0.144.5` Runtime, provider/backend thread, and
  `read-only` permission. Negotiated `workspace.edit.proposal.read-only` plus
  `permission.read-only` now covers `workspace/edit/proposal/latest`, exact
  `workspace/edit/proposal/read`, and Proposal-owned
  `workspace/edit/proposal/artifact/read`; the fixture verifies empty latest,
  `-32149` Proposal absence, `-32150` artifact/page absence, the 64 KiB page bound,
  Base64/chunk SHA-256, fixed-order domain-separated page identity, Session scoping,
  restart reads, and false authority in every public result. The complete Rust gate
  passes 27 AAP tests, 720 `aegisy-agentd` library tests with one ignored live
  fixture, 6 daemon-main, 10 threshold, 13 handshake Runtime, 17 Schema, 67 protocol,
  and 23 stdio/Codex tests. Strict workspace Clippy and formatting pass; strict
  OpenSpec validation and `git diff --check` are required below. Qt client coverage
  verifies capability gating plus Rust-compatible legacy preview and artifact-page
  identities. Qt render coverage verifies strict `0.2` create/update/delete/rename
  and exact legacy `0.1` projection, foreground auto-open, background unread state
  without focus theft, invalid authority/schema cache protection, Proposal
  invalidation of in-flight artifact reads, frozen Session/Proposal/file/generation
  response binding, zero-byte and 64 KiB page boundaries, UTF-8 characters crossing
  pages, verified intermediate UTF-8 prefixes, immediate rejection of irreparable
  continuation/overlong/out-of-range UTF-8 tails, Windows drive-prefix denial, and
  the absence of Approval/Apply controls.
- The schema-v19 Change reference slice adds an immutable
  `workspace-edit-proposal-reference/0.1` binding and a completed public
  `file-change` Item for each new Proposal. Focused Store fixtures prove the Proposal,
  artifacts/Blob references, Item plus internal `item.appended`, unchanged internal
  `workspace-edit.proposal-recorded`, public `item.completed`, cursor advancement,
  and binding commit atomically; an injected public-Journal insert failure restores
  every row count and both internal/public sequences, and removes uncommitted Blob
  references. Exact retry writes one of each row/event, drift conflicts, and an
  injected post-commit caller fault leaves the complete committed graph for an
  exactly-once retry.
- The v18-to-v19 migration fixture requires one WAL-consistent backup, preserves the
  exact legacy Proposal bytes/identity and `timeline_reference:null`, sets the legacy
  requirement marker to false, and fabricates no Item, public event, or binding. New
  v19 reads require exactly one binding and revalidate its canonical reference JSON,
  Item payload/hash/sequence, stored public envelope bytes/hash/anchor, and all false
  authority fields. Semantic binding tamper quarantines only the owning Session.
  A dedicated prune/restart fixture deletes the Journal envelope through the existing
  checkpoint path while retaining a readable Proposal, binding, and Item; the read is
  accepted only because the saved public sequence is at or below the validated floor.
  Deferred Item ownership also survives destructive Session projection rebuild, and
  final Session purge removes Proposal/reference/Item/events, resets public retention,
  and releases artifact references with the normal undo window.
- The real stdio decline fixture now finds the exact durable `file-change`
  `item.completed` and matches its Reference ID/Proposal ID to the restarted Store
  record before confirming the workspace is unchanged. Qt render coverage rejects a
  forged reference identity, unknown/path fields, true Apply authority, drifted exact
  Proposal responses, stale generations, focus theft, and cross-bound/unavailable
  reads. It preserves the latest cache while a validated explicit reference opens the
  bound historical Proposal in Changes with no Approval or Apply control. The durable
  Change reference slice is therefore present, but message/reasoning/review/image and
  complete Tool mappings plus genuine Approval/Apply/checkpoint authority remain
  absent, so tasks `7.5` and `7.6` stay unchecked.
- Workspace-edit apply tests cover same-directory staged create/update content,
  immediate optimistic base/target rechecks, no-clobber hard-link backups, all four
  operation kinds, reverse rollback after partial and complete multi-file commit,
  exact final SHA-256 values, sensitive/Git-ignore/symlink/existing-target denial,
  and cleanup of hidden transaction files. A stage-time external change is rejected
  before commit. A post-commit external rewrite is preserved instead of overwritten,
  returns authoritative path state, reports rollback incomplete, and retains the
  original backup as a named recovery artifact. Task 15.3 is complete as an internal
  library transaction only; AAP apply remains absent. Deterministic failure injection
  is covered by task 15.9, while real Windows filesystem execution remains external
  evidence.
- Workspace-edit overlap tests bind proposal and post-apply restore baselines to the
  validated edit/project/root identity and exact absent/SHA-256 path states. They
  cover all create/update/delete/rename source and target roles, disk create/delete/
  content changes, declared unsaved editor paths, sensitive/Git-ignore/symlink
  denial, forged apply/root/final-hash data, 512 KiB file bounds, and the 8 MiB
  aggregate hash-read budget. Reports contain hashes and stable conflict/resolution
  codes rather than source content. Task 15.4 is complete as a reusable internal
  read-only detector; checkpoint capture/restore and UI mutation remain later tasks.
- Git-checkpoint fixtures initialize a real dirty repository and prove that an
  isolated `refs/aegisy/checkpoints/*` plumbing commit retains exact touched-file
  preimages plus a bounded manifest while leaving the worktree and semantic index
  tree unchanged. The manifest separates staged, unstaged, untracked, declared
  unsaved, redacted-sensitive, `agent-only`, and `agent-on-user-base` state; a
  post-apply bind verifies ref/commit/tree/manifest/blob OIDs, root/gitdir, HEAD,
  branch, index tree, operation roles, and final hashes. Negative fixtures cover
  ref overwrite, tampering, index drift, stale bases, ignored/symlink/pending paths,
  unsafe refs, parent repositories, and external metadata roots. Git commands use a
  project-external absolute executable, cleared minimal environment, disabled hooks
  and prompts, and bounded output. Task 15.5 is internal only; no AAP mutation exists.
- Non-Git checkpoint fixtures persist exact touched-file preimages into a disjoint
  project-external SHA-256 blob/manifest store, reopen it as a new store instance,
  verify duplicate-blob coalescing, and bind all create/update/delete/rename roles
  to a real committed apply result. Descriptor, manifest, pointer, blob, root, store,
  mode, byte-count, and final-hash bindings are checked before use. Negative tests
  cover sensitive pending-path redaction, stale/ignored/pending/symlink paths, Git
  worktree refusal, overlapping storage, manifest/blob/pointer tampering, oversized
  preimages, and conservative object/byte admission limits. Every result carries the
  explicit weaker guarantee and four concrete limitations. Task 15.6 adds no AAP
  checkpoint/apply/restore method.
- Restore fixtures construct plans from both Git and non-Git verified preimages,
  group rename source/target under one operation selection, restore one operation
  before the remaining full set, recognize already-restored/no-op paths, and prove
  an unrelated later user file remains byte-identical. Conflict execution requires
  the exact reviewed current hash; missing confirmation, stale review, duplicate
  selection, ignored/policy-unavailable state, and pending editor state fail before
  mutation. Unix coverage additionally proves that full restore recreates a deleted
  executable with its executable class intact. Task 15.7 is complete as an internal
  sidecar surface; no restore AAP or Qt action exists.
- Workspace-edit format fixtures require `workspace-edit/0.2` descriptors and bind
  exact content bytes to UTF-8/BOM, none/LF/CRLF, and preserve/regular/executable
  intent. Legacy/missing descriptors, mismatched bytes, mixed or lone-CR endings,
  binary/NUL content, oversized proposals, and binary/mixed/oversized bases fail
  before mutation. Final and parent symlinks are denied without touching their
  targets. Unix fixtures prove BOM+CRLF byte preservation, executable create and
  promotion, executable preservation through update/rename, explicit demotion to
  regular, checkpoint mode-tamper rejection, and executable full restore. Windows
  explicitly rejects executable intent because this layer has no durable POSIX mode
  representation; a real Windows SDK/runner remains required and is not claimed by
  this macOS evidence. Task 15.8 adds no AAP/Qt mutation surface.
- Transaction failure fixtures inject partial stage writes, `StorageFull`, and
  permission denial at deterministic boundaries. They prove incomplete stages are
  removed before visible mutation and a post-commit permission failure restores
  exact bytes and permissions. A child test process exits immediately after an
  update commit without unwinding; its parent verifies the exact installed target,
  retained exact preimage backup, and stale-base rejection on retry. Existing
  fixtures cover stale-after-stage and rollback-incomplete after an external
  rewrite. Task 15.9 does not claim automatic crash-journal replay or substitute
  for real disk-quota, ACL, forced-termination, and Windows runner tests.
- Git-status fixtures parse bounded porcelain-v2 NUL records and branch headers for
  ordinary, rename/copy, unmerged, untracked, initial, branch, detached, upstream,
  and ahead/behind state. Real repositories prove canonical discovery from a nested
  project directory, staged/unstaged/untracked/rename classifications, detached
  HEAD, non-repository behavior, and a conflicting merge with `MERGE_HEAD`. The AAP
  contract reports `git-status/0.2` while retaining Qt's `path/status` fields. Git
  execution uses a project-external absolute binary, cleared minimal environment,
  fixed locale, disabled prompts/optional locks/fsmonitor/config/hooks, 2 MiB output,
  and 5,000 entries. Task 16.1 is read-only and adds no Git mutation method.
- Git-query fixtures exercise `git-query/0.1` overview, paged log, root-commit
  parsing, commit detail, and worktree/staged/commit diffs in a real repository
  containing a nested opened project. Full-OID and path traversal rejection, 2 MiB
  command bounds, project-prefix `:(top)` pathspecs, sensitive file and sensitive
  rename exclusion, sibling-path exclusion, and credential-bearing remote URL
  authority redaction are asserted. AAP coverage proves project binding and keeps
  stage/branch/create-commit/push methods absent. Qt render coverage requires the
  summary, four-column history, three-scope selector, icon refresh, read-only diff,
  and real diff context action. Task 16.2 adds no Git mutation surface.
- Internal branch-transaction fixtures bind create/switch/rename plans to canonical
  repository root, exact HEAD/current branch and target/source OID, clean porcelain
  state, bounded validated names, protected patterns, and linked-worktree occupancy.
  Execution fully replans before mutation and verifies final branch/HEAD. Real Git
  tests cover dirty/protected/stale/worktree denial plus injected rollback for all
  three operations. Task 16.3 remains unchecked and unreachable from AAP/Qt until
  permission and approval gates can authorize user-visible Git mutation.
- Internal dedicated-worktree fixtures bind plans and descriptors to canonical
  repository/common-dir and external storage roots, exact base HEAD/branch, unique
  path/branch, and session/child owner. Real Git tests cover create plus lock,
  health refresh, clean terminal-child cleanup eligibility, dirty/stale/overlap/
  reused/symlink/invalid-name denial, owner and branch tampering, missing/prunable
  state, pending editor/retention gates, and complete rollback after injected
  post-add and post-lock failures. Task 16.4 remains unchecked because no durable
  scheduler association or permission/approval-backed AAP/Qt create, remove, or
  integrate action exists.
- Internal staging fixtures start from a real workspace edit, Git checkpoint,
  apply result, and bound application rather than fabricated Agent ownership.
  Content-free plans bind the checkpoint object graph, HEAD/branch, current index
  tree/file identity, exact Agent results, path/mode state, and stable hunk IDs.
  Real Git tests prove selected Agent hunks merge into the existing index while a
  distant user staged hunk remains staged, a user unstaged hunk remains excluded,
  an unselected Agent hunk remains unstaged, and the worktree remains byte-identical.
  Additional fixtures cover create/delete/rename, mode-only selection, overlapping
  hunk refusal, stale index, pending editor, competing lock, capture-time/later
  staged delete protection, exact post-install rollback, and preservation of an
  external index rewrite with rollback-incomplete evidence. Task 16.5 remains
  unchecked and unreachable from AAP/Qt pending permission, approval, durable
  events, hunk review, and unstage product controls. Windows cross-check still
  stops in existing Tree-sitter C before this module because Windows SDK headers
  are unavailable on the macOS host.
- Internal commit fixtures continue the real edit/checkpoint/apply/stage authority
  chain. A three-tree preview uses the pre-Agent index as merge base, HEAD as ours,
  and post-Agent index as theirs; the resulting exact patch/commit contains only
  the Agent delta while a same-file user staged hunk remains in the real index and
  a user unstaged hunk remains only in the worktree. Raw commit-object verification
  proves exact tree/parent/message/author/committer/timezone metadata and the failing
  executable hook marker proves disabled-hook plumbing does not execute hooks.
- Additional commit fixtures cover message-source and identity policy, detected
  hooks/custom hooksPath without path disclosure, Run/sign blockers, custom merge
  driver refusal without marker execution, invalid control characters/email, stale
  index receipt, compare-and-swap ref update, exact post-update rollback, and
  preservation of an external ref rewrite with rollback-incomplete evidence. The
  final internal result is `git.commit.completed`, but task 16.6 remains unchecked
  until permission/approval-backed AAP/Qt review, durable event storage, sandboxed
  hook output artifacts, secure signing, and Windows execution evidence exist.
- Workflow-state fixtures cover execution-complete `git-workflow-plan/0.2`
  requests for stash, three merge modes, rebase, single-parent cherry-pick, and
  exact-generation abort/continue. Plans bind roots, HEAD/branch/index, target OIDs,
  stash ref, predicted behavior, message source, identity/timezone, hook/signing
  policy, pending editor state, and value-free hazards. Tests reject ambiguous or
  missing merge metadata, impossible fast-forward, already-contained targets,
  merge-commit cherry-pick without mainline, invalid message/identity, custom
  drivers, protected rebase, stale plan/OID/generation, and unsafe dirty state.
- The private project-external `git-workflow-record/0.2` store survives reopen,
  detects payload/hash/path/symlink/semantic tampering, and serializes transitions
  through an OS advisory lock plus generation/record CAS. Its durable attempt journal
  records prepared/dispatching/observed/recovered phases without source content.
  Real conflicts retain exact stage mode/OID evidence while sensitive names are
  count-only; foreign/unknown operations are inspect-only.
- Authorization hashes both the exact record and freshly regenerated action plan.
  Separate short-lived allow-once permission and approval decisions are required
  for every medium/high action, including a newly classified high-risk abort, and
  replay/forged/stale scopes fail. The internal executor then proves real stash,
  fast-forward and merge-commit, rebase, cherry-pick, conflict continue, and abort.
  Hooks are bypassed only under explicit Disabled policy; a failing hook marker is
  not executed. Restart fixtures recover live conflicts, exact completed fast-
  forwards, and unknown dispatches conservatively. Task 16.7 remains unchecked:
  production authority/issuer, AAP/Qt flow, complete session projection, sandboxed
  hooks, signing, conflict UI, and Windows runtime evidence are absent. A partial
  `WorkbenchStore` now provides a private project-external SQLite WAL schema with
  full synchronous writes, application/schema integrity checks, durable decision
  rows, atomic decision consumption plus `approval.consumed`, and typed event
  replay with payload hashes and sequence-gap detection. Six focused store tests
  cover reopen/application identity, atomic rollback, replay/integrity, and direct
  authority rechecks. It remains internal and
  does not claim the production authority, migrations/backup/compaction, or UI
  workflow.
- The internal `permission-profile/0.1` boundary defines Chat, Read Only,
  Workspace Write, Developer, and Full Access profiles plus managed-policy
  intersection. Effective checks cover canonical writable/read-only roots,
  sensitive and denied paths, symlink components, shell wrappers and executable
  allowlists, network host patterns, extension IDs, browser hosts, and background
  execution. A private Git decision issuer accepts permission decisions only from
  a matching profile authority and explicit approvals only with a distinct,
  bounded user-gesture ID; read-only profiles and managed denials fail before any
  SQLite row is written. This remains an internal policy/issuer foundation and is
  not connected to AAP, Qt, native execution, or a production user-gesture bridge.
- At the schema-v14 milestone, the SQLite Store carried project/session/Blob/retention/job/
  lease/notification-outbox/model-profile metadata and turn/item projections:
  canonical project roots and access, Chat/Work session mode, project binding,
  environment identity, `new`/`resume`/`fork` lineage, active/archived/failed/
  interrupted status, and archive/unarchive transitions. Turns bind to an active
  session, support bounded idempotency keys and input hashes, and transition from
  started/running to terminal states. Items use a session-local monotonic sequence,
  bounded redacted JSON payloads, content hashes, and turn binding. Replay detects
  sequence gaps and payload tampering; terminal turns reject late items. Work sessions cannot be
  created without a project; lineage parents must match project and mode; invalid
  rows are rejected before insertion. Reopen tests verify metadata durability and
  transactional v1-through-v13 source migrations into v14. The v3 path rebuilds the event table
  with nullable project binding while preserving every existing event field, hash,
  and sequence; a new Chat session then proves a typed event can carry no project.
  The complete extensions/checkpoint and scheduler/job recovery schema remains
  unchecked; model profiles have only the authority-free projection described
  above. Runtime
  integration now persists project/session metadata, Preview turns, and completed
  Codex timeline items under the Qt host's platform application-data Workbench root;
  `AEGISY_WORKBENCH_DATA_ROOT` remains an explicit developer/test override;
  `session/read` replays the bounded durable item projection after sidecar restart
  and labels the runtime as a read-only durable replay. `project.created`, session
  create/title/status, Turn create/terminal, and sanitized Item append events commit
  in the same transaction as their projection. An injected event-insert trigger proves
  a title mutation and event sequence allocation both roll back. Fresh sessions register
  event-source version 1; v3-migrated sessions have no marker and remain explicitly
  legacy/non-rebuildable. Richer runtime/environment reconstruction and future
  mutation event coverage remain unchecked.
- At that milestone, every supported v1/v2/v3/v4/v5/v6/v7/v8/v9/v10/v11/v12/v13
  source received a WAL-consistent SQLite Online Backup before migration. The
  standalone DELETE-journal backup and bounded JSON
  manifest bind source/target schema, application ID, exact bytes, SHA-256,
  creation time, and integrity state under a private no-clobber directory. Admission
  enforces a 1 GiB file limit, 16 retained evidence sets, a 256 MiB free-space
  reserve, and bounded inventory/manifest reads. Valid backups, unmanifested files,
  invalid manifests, interrupted temporary files, and tampered evidence are all
  handled conservatively; uncertain evidence is reported and never deleted.
- Those migration fixtures proved v1/v2/v3/v4/v5/v6/v7/v8/v9/v10 state preservation, full required-schema
  validation before `user_version` commit, schema-collision rollback with the
  original v4 state and backup intact, safe re-entry after an uncommitted migration,
  exact preservation of random corrupt database bytes, low-space rejection, backup
  tamper detection without deletion, and refusal to downgrade a newer schema.
  Failure starts a content-free read-only Runtime before Codex launch. Initialize
  advertises only four recovery/read-only capabilities, ordinary AAP methods return
  `-32120`, and status/export contain no database content, project paths, credentials,
  raw SQLite errors, or stored display data. Tasks `5.6` and `5.7` are complete.
- The schema-v6 store retains the schema-v5 durable Blob design and keeps project-external SHA-256 objects in a
  private, sharded, no-clobber filesystem and keeps exact bytes, media/kind,
  content reference, session/project/source owner, hashed bounded metadata,
  access/verification time, state, and retention deadline in SQLite. Command
  artifacts commit in the same `IMMEDIATE` transaction as their Item projection
  and typed event; injected event failure rolls back the database rows and removes
  the newly committed object. Changes preview content/diffs persist as an atomic
  batch and both command output and patch paging survive sidecar restart with the
  original session/project/edit scope. Media fixtures cover patch, binary image,
  diagnostic JSON, workspace-edit, generic, and empty artifacts.
- Blob admission enforces 16 MiB/object, 8,192 objects, 512 MiB total, and a
  256 MiB free-space reserve where capacity is available. Every read verifies
  file type, exact size, and SHA-256. Release retains at least a 24-hour undo
  window; GC refuses active, not-yet-expired, missing, corrupt, unknown, or
  unregistered objects. Fixtures cover deduplication, cross-session denial,
  reopen, metadata-secret rejection, low-space rejection, size/hash corruption,
  missing files, rollback cleanup, active-reference retention, grace-window GC,
  unknown orphan preservation, and v4→v6 migration through the Blob schema step. Task `5.3` is complete.
- The session-projection consistency and rebuild layer is internal and bounded. It checks
  project and lineage binding, turn hash descriptors, item sequence and same-session
  turn ownership, item payload integrity, event sequence cursor state, and event
  payload integrity without returning content or paths. A focused fixture first
  proves a healthy projection, then injects a cross-mode lineage parent, deleted item,
  cross-session turn binding, tampered item/event payloads, and a stale event cursor;
  every failure becomes a stable issue code and requires either a proven automatic
  projection rebuild or read-only recovery. Initial durable `session/read` enforces
  this report, while older-page requests retain page-local hash/sequence validation.
  Typed metadata/Turn/Item events replay into a separate candidate and compare exactly
  with the current projection. Repair requires a complete registered source and
  `event-projection-mismatch`, revalidates a
  SHA-256 identity of the whole event stream under an `IMMEDIATE` write lock, atomically
  replaces session/turn/item rows, and appends content-free rebuild audit evidence.
  Fixtures repair metadata, lineage, cross-session Turn binding, and missing Item rows;
  a post-review event append rejects a stale candidate. Deleting a registered source is
  detected rather than mistaken for legacy. Session verification now also checks
  durable reference ownership, projection owners, metadata integrity, and referenced
  file size/hash. A separate bounded content-free scan reports missing/corrupt objects,
  dangling references, malformed entries, and unregistered files while preserving
  every uncertain orphan. Startup performs a union scan of session rows, registered
  sources, and event-only streams before Codex launch. It caps identities at 10,000
  and each Turn/Item/event/Blob-reference class at 100,000, repairs complete sources,
  reconstructs simultaneously missing parent/child session rows in dependency order,
  and escalates scan/query/limit failures into whole-store read-only recovery.
  Pre-commit validation repeats project, lineage, Blob owner/content, source count,
  and complete stream-hash authority.
- Uncertain sources enter per-session quarantine. Store methods reject metadata,
  Turn, Item, Blob/Artifact, Git authorization/event, and GC mutations before their
  transactions; Runtime blocks session-bound AAP writes while retaining cancellation
  and terminal cleanup. `runtime/projection-recovery/status`,
  `session/recovery/status`, and `session/list.recovery_required` expose content-free
  state. Qt renders distinct whole-store, automatic-rebuild, aggregate quarantine,
  and current-session banners, disables unsafe composer/session/terminal/workspace
  controls, and keeps healthy sessions usable. Render tests exercise each state.
- Fixtures repair title/Item drift on first replay, rebuild missing parent/child rows
  at startup, preserve tampered event authority without an audit rewrite, block
  quarantined Store and AAP mutation plus Blob GC, and enter content-free whole-store
  recovery when startup limits are exceeded. Project creation and additional roots
  now append one bound project stream; startup scans projects before sessions and
  reconstructs an entirely missing two-root project with independent read/write
  scopes before its Work session is verified. Stored absolute canonical root text
  remains replayable while a directory is offline, whereas tampered project authority
  quarantines the project and every bound session without rewriting evidence.
- Project and session rebuild transactions have both deterministic storage-failure
  and real process-exit evidence. SQLite insert-abort triggers fire after destructive
  projection clears and prove the complete previous project/root or session/Turn/Item
  projection remains visible with no rebuild audit. Separate child test processes exit
  directly from the actual `IMMEDIATE` transaction after the clears; a raw reopen sees
  the prior complete projection, and normal startup then rebuilds it from the unchanged
  event stream. Task `5.4` is complete. Migration diagnostic export remains implemented
  under `5.6`.
- Database maintenance and low-space fixtures verify the shared write boundary rather
  than one mutation type. All ordinary `IMMEDIATE` transactions first require the
  256 MiB reserve plus 8 MiB transaction headroom and remain below a 1 GiB combined
  database/WAL ceiling; rejection leaves session, source-registration, and event rows
  absent. A verified artifact remains readable under low space without an access-time
  write, while release and other mutations fail content-free. A competing writer holds
  `BEGIN IMMEDIATE` and proves the configured two-second busy wait returns below five
  seconds without a partial row.
- Configuration fixtures assert WAL, `synchronous=FULL`, 1,000-page auto-checkpoint,
  16 MiB journal retention, and database page ceiling. Explicit maintenance requires
  `reserve + headroom + 2*database + WAL`, rejects one byte below that boundary, then
  truncates WAL, runs `VACUUM`, clears the freelist, and passes `quick_check`. Projection
  recovery blocks maintenance. Together with the WAL-consistent pre-migration backup
  and recovery fixtures under `5.6`/`5.7`, this completes task `5.5`.
- The read-only `session/list` AAP method now returns bounded project/mode/archive
  filtered metadata from either the in-memory Runtime or the durable SQLite store.
  Qt consumes the result in the left session rail, prioritizes current-project Work
  sessions, and invokes `session/read` when a row is selected; a restart fixture and
  the two Workbench render targets verify durable timeline replay. `session/read`
  now returns the newest 100 items by default, accepts 1-200, exposes item sequence
  metadata, and pages backward through strict `before:<sequence>` cursors. Protocol
  fixtures verify exact non-overlapping newest/middle/oldest pages and reject malformed,
  non-canonical, out-of-range cursors and invalid limits. The SQLite restart fixture
  verifies the same boundary from durable storage. Qt exposes a hidden-until-needed
  load-more control, prepends older items, and restores the prior scroll anchor.
  Task `6.8` is complete; project relink and full event-projection rebuild remain
  separately unchecked under `6.4` and `5.4`.
- AAP `session/search` now performs a bounded SQLite-side Session query by project,
  exact branch/model/runtime/status, title, or a combined title/approved-transcript query.
  Transcript matching is restricted to `message` Items with `user`/`assistant` roles
  and visible `text`/`content`/`output`/`diff` fields; diagnostic and command payloads
  do not match, and the Runtime does not hydrate every transcript. Results carry
  Runtime and Workspace binding metadata, matched-field evidence, purged-session exclusion, a
  strict `after:<updated-at>:<session-id>` cursor, and a 100-result cap. Additive
  SQLite schema v13 migration creates and verifies the indexed
  `session_workspace_bindings` projection plus status/order, model/runtime binding,
  branch identity, and transcript ownership indexes through the existing
  WAL-consistent backup gate. Work creation commits the Runtime and Workspace rows
  with `session.created`, `session.runtime-bound`, and `session.workspace-bound` in
  one transaction. The Workspace event/table carries project/root identity, Git
  state, safe branch display plus exact SHA-256, HEAD/worktree identities, and false
  dedicated-worktree/permission flags without raw repository paths.
  Capabilities `session.workspace-binding.read-only` and `session.search.branch`
  negotiate the additive response and exact branch filter.
  Workspace observation happens before provider Session creation; if the later
  atomic Store transaction fails after Codex start, Runtime best-effort archives
  that newly created provider thread rather than silently retaining an unbound
  continuation.
  Existing v12 Work Sessions are not backfilled from current Git state: resume fails
  closed with `-32146` until an explicit fork/rebind captures reviewed workspace state.
  The Qt left rail debounces title/transcript search, scopes Work to the current
  project, renders explicit empty results, and restores the recent list when cleared.
  Store and protocol fixtures cover approved-field isolation, branch/model/runtime
  filtering, cursor canonicalization, v12-to-v13 backup migration, event-failure
  rollback, final-deletion purge, semantic tamper rejection, restart read/search/resume,
  and branch-drift rejection against a real Git repository. Task `6.6` remains unchecked because
  complete indexed-text scale, branch-filter UI, dedicated-worktree/model-control
  integration, and cross-platform evidence remain open.
- AAP `operation/reconcile` now validates content-free event/process/workspace/Git
  evidence through `operation-reconciliation/0.1` and appends a metadata-only
  `operation.reconciled/0.1` event to the session stream when durable storage is
  configured. Identical evidence is idempotent; the latest result per operation is
  validated from the event hash and survives Runtime restart. Unknown, running, or
  blocked results reject later session-bound mutations with `-32132`, while a newer
  authoritative terminal review clears the gate. Capability negotiation advertises
  `operation.reconciliation`; store and protocol fixtures cover persistence,
  restart blocking, idempotency, and unblocking. Task `6.9` remains unchecked because
  the current method does not perform authoritative host probes automatically and
  exposes no recovery or recovery-action controls; Qt review is limited to an
  explicit, identity-checked turn-only probe/reconcile flow.
- Capability `operation.reconciliation.probe` now exposes read-only
  `operation/probe`. It resolves only a registered root through a Work session,
  hashes bounded visible workspace metadata, reads the existing structured Git
  status query, and observes runtime-owned turn/terminal state. Probe responses
  contain state labels and snapshot hashes but no content, arbitrary paths, or
  caller-selected PIDs. Event state may be caller-supplied or derived from the
  bounded durable turn/Git mappings described below; the probe does not persist,
  approve, mutate, or recover an operation. Task `6.9` remains unchecked until
  complete startup discovery, authoritative event sourcing, Qt review, and
  recovery actions are integrated.
- Durable Runtime startup now scans and validates the latest
  `operation.reconciled/0.1` event per session/operation pair into a bounded
  cache; the SQLite event stream remains authoritative for request-time gating.
  A malformed, missing, or over-limit scan cannot be interpreted as a safe
  operation result. This is partial startup discovery and does not complete task
  `6.9` without automatic operation source registration, Qt review, or recovery.
- When `operation/probe` omits `event`, durable Runtime derives the latest
  validated registered `turn.created`, `turn.completed`, `turn.failed`, or
  `turn.interrupted/cancelled` state and the existing `git.workflow.*` lifecycle
  state from the session event stream; prepared/dispatching/in-progress Git
  events are only running evidence, completed/failed/aborted are terminal
  evidence, and conflicted/recovered remain unknown. Explicit event values
  remain caller-labelled. Fixtures prove durable Turn and Git lifecycle mapping,
  while malformed Git payloads remain rejected without inferring success. Other
  operation event families remain unknown until their authoritative sources are
  registered.
- Capability `operation.reconciliation.status` exposes a read-only
  `operation/status` snapshot while a session is blocked. It returns only the
  bounded review summary and `recovery_action_available:false`; it cannot clear
  the gate or invoke recovery. Qt renders the same summary and, for a blocked
  turn, can explicitly collect `operation/probe` evidence and record a validated
  `operation/reconcile`; malformed, stale, or unsupported operation responses
  remain fail-closed. Render coverage verifies the blocked, review, and cleared
  transitions.
- The manual session-compaction checkpoint AAP methods are now reachable from the
  Qt session context menu. Create collects bounded user-authored summary fields
  and optional preservation instructions; read accepts an explicit checkpoint ID;
  revise creates a new immutable checkpoint from an exact source Review ID while
  preserving the old object and recording a validated `supersedes` descriptor.
  Both results and revision results render in a plain-text read-only dialog and require
  `activation_available:false`, `provider_compact_invoked:false`, matching session
  identity, and a non-empty review ID before display. Rust protocol/store fixtures
  cover conflicting IDs, stale source identity, idempotent revision, restart replay,
  and tampered lineage; the Qt render fixture covers client request serialization and
  revision signal dispatch. No activation, provider compact, startup compensation, or
  model-generated summary producer is exposed;
  task `6.10` remains unchecked.
- The partial `context-manifest/0.1` foundation is emitted with `turn/start` when
  structured context is present. It records only bounded source/kind/priority/
  trust/hash/token/freshness/inclusion metadata, labels stale file revisions,
  exposes `turn.context.manifest`, and never includes attachment text. Rust unit
  and protocol fixtures verify deterministic metadata, conservative token sizing,
  truncation, and absence of a `content` field; task `17.1` remains unchecked until
  instruction, budget, inspector, tokenizer, and provider-scale consumers exist.
- The partial instruction-discovery/0.1 foundation exposes read-only
  workspace/instructions. It binds a registered project root, applies the
  deterministic weakest-first precedence managed > user > nested (closer depth
  wins) > project, and returns only bounded provenance, precedence, hash,
  revision, token, freshness, inclusion, truncation, and rejection metadata unless
  the caller explicitly requests bounded content. Managed/user sources are
  restricted to path-only environment configuration; project sources use the
  existing sensitive, symlink, built-in-ignore, and Git-ignore policy. Content is
  always marked untrusted-data and cannot authorize permissions, commands,
  Hooks, or network. Unit and protocol fixtures cover nested target selection,
  deterministic ordering, case collisions, secret/control/symlink denial,
  stale-read checks, and size/count bounds. Work-mode turn/start now appends up
  to eight valid discovered instructions after explicit user context, through
  the same bounded context preparation and manifest; secondary-root attachments
  do not select a primary-root instruction chain. Task 17.2 remains unchecked
  until durable managed/user configuration, complete exclusion/budget reporting,
  policy/trust review, context inspection, and cross-platform evidence are
  complete.
- The partial context-budget/0.1 allocator is emitted with each prepared
  turn context. It deterministically scores instruction precedence and pinned
  context, classifies existing task-state/recent-turn/tool-result/search and
  repository-map consumers, allocates within a 64 KiB total/16 KiB per-item hard
  bound without reordering rendered context, and returns requested/allocated bytes,
  class, priority score, inclusion, and content-free reason metadata. Unit fixtures
  cover priority ordering, consumer classes, excluded entries, and hard bounds; the protocol
  fixture verifies the budget schema. Task 17.4 remains unchecked until these classes have
  authoritative producers, tokenizer/provider window authority, and scale evidence use
  this allocator.
- The partial `tokenizer/0.1` contract makes the current byte estimate explicit as
  `unknown-utf8-four-byte` with `authority: conservative-unknown`, `exact:false`,
  and `provider_window_authoritative:false`. Budget entries and the aggregate plan
  now expose conservative estimated token counts without returning source text or
  asserting provider-window fit. Unit fixtures cover metadata and overflow-safe
  rounding; model-specific tokenizer adapters remain unavailable, so task 17.5 stays
  unchecked.
- The Qt context-inspection dialog renders the tokenizer authority label and one
  conservative estimated-token value per budget entry, using 64-bit JSON conversion.
  The complete desktop build passes after this change. The focused
  `agent_workbench_render` CTest was killed by the host at startup (0.44s) without
  assertion output, so visual evidence remains pending a lower-resource runner.
- The Agent header now renders Chat/Work mode, project name, persisted primary-root
  workspace, Runtime readiness or recovery state, provider/model, the fixed
  `read-only` permission profile, the Session-bound branch, and selected context
  count. Qt accepts only the content-free `session-workspace-binding/0.1` projection.
  The live read-only Git overview is comparison evidence; a mismatch marks branch
  drift instead of overwriting the Session binding. The complete desktop build and
  CTest `agent_runtime_protocol` pass; the focused render fixture locates the strip
  by a stable object name and asserts its empty state, real Git branch, `root-1`, and
  active Runtime/Workspace binding. The focused render process was still killed by
  the host at startup (0.45s) without assertion output, so executable visual evidence
  remains pending a lower-resource runner.
- Qt now caches one strictly bounded Runtime binding per Session from
  start/resume/fork/read/search responses. The active mode/session alone drives the
  model display and execution strip; adapter/version are required, provider/model
  are optional bounded labels, and permission must equal `read-only`. Invalid data
  removes the binding and produces an explicit unknown/read-only gate. The protocol
  fixture proves `session/read` returns the same Runtime binding created for each of
  two isolated Sessions. All 57 protocol tests, strict Clippy, the complete desktop
  build, and CTest `agent_runtime_protocol` pass. The render fixture additionally
  asserts `preview`, `local / deterministic-echo`, and `read-only`, but its process
  was killed by the host at startup (0.47s) before any assertion output.
- AAP turn/context/inspect is a read-only preflight that reuses the exact
  instruction discovery and budget preparation path for the bound session.
  It returns context-inspector/0.1, manifest/budget metadata, and explicit
  content_included:false, model_started:false, and persisted:false flags;
  protocol coverage proves instruction/source bodies never enter the response.
  Qt exposes a preflight action for an existing session and renders a read-only
  source/type/trust/size/status/reason table; the Workbench render fixture
  exercises the control and metadata-only result. Unchecked client context is
  represented as an explicit exclusion marker. Task 17.6 remains unchecked until complete context classes,
  redaction/exclusion explanations, provider/tokenizer authority, and
  cross-platform evidence are complete.
- Session metadata management now has bounded `session/title`, `session/archive`,
  and `session/unarchive` AAP operations. Store timestamp guards prevent stale
  projection rewrites; Runtime checks reject archival during an active turn or
  running/stopping terminal, and archived sessions reject new turns, terminals,
  and workspace-edit previews. The Qt session context menu exercises rename,
  archive visibility, and restore in the Workbench render fixture. Resume, fork,
  and Codex thread lifecycle mapping remain unchecked; immutable create, title,
  archive, unarchive, and deletion lifecycle events now pass store replay/rebuild fixtures.
- Schema v6 adds project/session retention overrides and two-phase deletion state.
  `session-only` and descendant-lineage previews bind the complete bounded member,
  status/update, Turn, Item, event, and Blob-reference impact to a SHA-256 plan;
  only the first 200 affected sessions are displayed, while the hash covers up to
  10,000 members. Scheduling repeats the plan under an `IMMEDIATE` write lock,
  requires a 24-hour-to-30-day undo window, and freezes all selected sessions.
  Runtime checks every selected descendant for active Turns and running/stopping
  terminals; Store policy sweeps additionally protect durable live Turns, issued
  approvals, recovery quarantine, and the host-provided live-session set.
- A pending deletion remains readable but rejects session metadata, Turn, terminal
  input/start, edit-preview, Blob, approval, and authorization writes. Undo deletes
  the pending plan only after immutable audit append succeeds. Due purge atomically
  removes session content and authority rows, releases Blob references, extends
  physical retention by at least another 24 hours, and installs a minimal archived
  tombstone so existing child lineage foreign keys remain valid. Normal list/read
  paths and startup recovery hide/exclude purged tombstones, while host maintenance
  evicts resident timeline, command-artifact, edit-preview, and terminal caches before
  they can bypass durable deletion state.
- AAP now exposes `session/delete/preview`, `session/delete/schedule`,
  `session/deletion/status`, `session/delete/undo`, retention policy read/set/remove,
  and host-triggered retention/GC maintenance. Qt provides delete-scope choice,
  exact impact review, destructive confirmation, a seven-day undo period, pending
  row/banner state, disabled mutation controls, Undo, and project/session policy
  dialogs. Six focused Store fixtures, one Runtime deletion/maintenance/restart
  fixture, one external AAP contract test, and the Workbench render flow cover stale
  plans, rollback, live-work blocking, pending read/freeze, undo, purge, delayed Blob
  GC, policy precedence/removal, tombstone restart, and Qt state. Task `5.8` is complete.
- Portable session fixtures verify export category/warning previews, stale-preview
  rejection, a second secret scan, registered project-root replacement, and removal
  of provider response IDs, encrypted/hidden reasoning, cache/continuation handles,
  environment identity, and local artifact/content references. Version, content-hash,
  sequence, duplicate Item ID, and reintroduced opaque-field tampering fail with
  stable errors before import writes. The package contract and its 4 MiB/2,000 Item
  limits are documented in `docs/PORTABLE-SESSION-FORMAT.md`.
- Import fixtures prove Work requires an active target project, previews source
  Session/Item collisions, enforces `reject` inside the SQLite write transaction,
  and makes `copy` allocate a new Session plus deterministic remapped Item IDs. A
  readable same-store source with matching mode/project creates fork lineage; source
  history remains unchanged. Injecting failure on `session.imported` proves Session,
  Item, event, and projection-source rows all roll back. Candidate rebuild, normal
  consistency verification, raw reopen, and restart replay accept the validated audit
  event without changing projected Session/Turn/Item state.
- External AAP coverage verifies `session.portable.export/import` capability
  negotiation, export preview/hash commit, import preview, reject/copy behavior,
  tampered-package error mapping, immediate history reads, and restart consistency.
  Qt adds a visible import command and per-session export command, complete content
  and collision review, 4 MiB file admission, `QSaveFile` atomic output, target-project
  binding, and imported-session selection. Both Workbench render targets pass with
  isolated temporary data roots. Task `5.9` is complete; portable history explicitly
  omits provider continuation and does not complete model/runtime switching tasks.
- Task `5.10` adds one persistence-secret invariant shared by SQLite projection/event
  writes, Item replay, event replay, projection-source replay, canonical roots,
  display/title text, identity/identifier fields, and durable Blob metadata. Nested
  credential-like fields and recognized API-key/JWT/authorization values are rejected
  before event sequence allocation; user/model Item text is redacted first, then the
  sanitized bytes and hash are used identically for `items.payload_json` and
  `item.appended` events. A direct event-gate failure after a transient projection
  update rolls the transaction back with no sequence, event, or projection side effect.
  Project/session display rejection, nested JWT redaction, Blob metadata rejection,
  and zero-row assertions are covered by Store fixtures. A legacy source-less session
  export proves historical title re-redaction, while an injected hash-consistent secret
  event is refused during replay and quarantines its session on reopen.
- The Qt host now sanitizes the process environment before starting `aegisy-agentd`.
  A dedicated CTest fixture proves API keys, Aegisy login/refresh tokens, cloud
  credentials, and authenticated proxies are absent while the Workbench data root and
  ordinary model settings remain. The sidecar client has no secure-storage/token API
  and therefore cannot pass raw desktop credentials through AAP parameters.
- The complete verification run for this batch passes 232 Rust unit tests, 22 AAP
  protocol tests, 3 stdio/Codex contract tests, 16 desktop CTest tests, strict Clippy,
  `cargo fmt --check`, `git diff --check`, and `openspec validate --strict`. Task `5.10`
  is complete; provider-specific secret registration and clean Windows package runtime
  evidence remain later security/release work.
- Task `6.1` now derives project root identity from filesystem metadata rather than
  canonical path text alone. On Unix it binds device/inode; the Windows implementation
  binds volume serial/file ID, and unsupported platforms use a deterministic path
  fallback. `project/open` returns the original project ID for a moved root with
  `availability=moved`, candidate root, and `relink_required=true`; it returns the
  original project as `unavailable` for an exact missing saved root. No new project row
  or trusted root binding is created in either case. Legacy path-hash identities are
  migrated atomically through a typed project event, and project replay applies the
  migration. A runtime fixture renames a real directory, reopens both moved and missing
  paths, checks the stable ID/candidate metadata, and confirms only one durable project
  remains. Explicit relink mutation and navigation UI remain task `6.4`.
- User-save/watch regression coverage configures a live directory watch before an
  atomic user save and proves the next poll is empty, while the existing external
  write fixture still produces a stale-revision conflict. The full Qt render flow
  passes repeatedly without self-save changes being mislabeled as external edits.
- The Windows ConPTY module and its test targets pass isolated
  `x86_64-pc-windows-msvc` check and Clippy from macOS. The Windows packaging
  workflow now runs the full Rust tests and Clippy before packaging and requires
  `aegisy-agentd.exe` in the distribution. Task 14.2 remains incomplete until that
  runner executes its ConPTY/Job Object tests or equivalent clean-VM evidence exists.
- Session-environment tests cover allowlisted inheritance, canonical PATH filtering,
  project-directory removal, case-insensitive credential masking, dangerous loader
  variable rejection, value/count limits, deterministic session identity, explicit
  terminal derivation, and value-free AAP metadata.
- Two hundred and ninety-eight Rust sidecar unit tests, fifty AAP protocol tests,
  eleven sidecar stdio/Codex contract tests, and sixteen desktop CTest tests pass;
  Clippy passes with warnings denied.

Known limitations:

- Split groups are a Monaco capability; the emergency Qt text fallback remains a
  single editor.
- Command-output artifacts remain runtime-memory-only until the durable event/blob
  store is implemented. Redaction is deliberately conservative and heuristic;
  future secret-provider registration remains part of the security-governance
  milestone. One Codex JSON message is rejected above 4 MiB rather than streamed
  through a second framing protocol.
- Search and index cancellation immediately supersede the client result and ignore
  late responses. The current synchronous sidecar may finish the already bounded
  active search page or index request before it reads the queued cancellation.
- Repository indexing is currently file-level incremental and bounded to the
  workspace scan limits, 8 MiB of newly parsed text, 20,000 symbols, and 10,000
  dependency edges. Dependency targets are syntax-derived import/include labels,
  not language-server-resolved definitions. Semantic definition/reference support
  remains a separate language-server path rather than part of the syntax index.
- Repository-map token usage is conservatively estimated as four Unicode
  characters per token and is not yet connected to the turn-wide context allocator
  planned in 17.4.
- Language servers are discovered from fixed command names or explicit path-only
  environment overrides and are not bundled by this milestone. This macOS host
  has end-to-end evidence for clangd; rust-analyzer, Pyright, and TypeScript
  Language Server still require installed-server and clean Windows validation.
- LSP requests are synchronously bounded to 4 MiB frames, 512 KiB documents, 500
  results, and fixed startup/request/diagnostic timeouts. Server-initiated
  `workspace/applyEdit` is denied, process credentials are scrubbed, and relative
  or project-root `PATH` executables are rejected, but the
  external language-server binary is not yet OS-sandboxed. Rust analysis therefore
  runs in restricted standalone mode and intentionally lacks full Cargo workspace,
  dependency, proc-macro, build-script, and compiler-check precision.
- Observed diagnostics and their normalized raw artifacts are intentionally
  in-memory only. Process restart durability remains a later event-storage
  milestone; build/test/lint diagnostic parsing remains task 14.8. The raw artifact
  is the filtered Aegisy authority payload, not the original unfiltered LSP wire
  message.
- Terminal excerpts now come from the real runtime-owned PTY/xterm surface. Git diff
  context and Git commit/diff pins come from the filtered read-only Git query rather
  than fabricated text. macOS PTY and named lifecycle are runtime-verified, while
  Windows ConPTY is implemented but awaits Windows execution.
- The internal `pinned-context/0.1` contract validates content-free descriptors for
  every 17.3 source kind. Unit fixtures cover kind support, project/duplicate
  binding, deterministic identity, unsafe absolute/parent/network references,
  secret-shaped metadata, item size, and aggregate size bounds. The runtime now
  opens `pinned-context-store/0.1` beside the Workbench store and exposes
  metadata-only AAP list/save/remove methods when it is healthy. A successful
  mutation appends a separate content-free `project.pinned-context-updated/0.1` event
  after object publication; project projection replay validates its set/object
  identities, bounds, and `content_bodies_persisted:false` invariant. Protocol
  coverage verifies project/root/session scope, restart recovery, event replay,
  idempotent persistence, compare-and-swap stale-write rejection, and that
  responses contain no body. Cross-resource event/object/Blob atomicity remains
  incomplete. Standard `*:sha256:` Blob references are checked through a
  read-only metadata query for active project/session ownership and exact
  hash/byte identity; no Blob body is read and no access timestamp is updated.
  Capability `turn.context.pinned-selected` now binds an explicit selected file/
  selection-pin ID list to the exact current set identity for both
  `turn/context/inspect` and `turn/start`. The shared resolver rechecks
  project/session/root scope, rereads the file under workspace policy, compares
  raw-byte SHA-256 and revision, marks drift stale, and for selections extracts
  only bounded metadata line/column ranges while keeping inspection metadata-only.
  Session-owned artifact pins additionally resolve only validated command-output
  text references with matching UTF-8, byte count, and SHA-256; the protocol
  fixture verifies durable Blob reload after Runtime restart and turn assembly.
  Project/root-bound diagnostic pins resolve only normalized
  `diagnostic-raw:sha256:` content from the authoritative in-memory DiagnosticStore
  after media-type, reference, SHA-256, and byte-count validation. Clearing that
  source, as occurs across Runtime restart or eviction, makes inspection fail closed;
  the persisted descriptor is never treated as a body.
  Session-owned terminal pins use a strict
  `terminal-excerpt:<terminal>:<generation>:<start>:<end>` reference. Assembly
  re-reads the Runtime-owned retained PTY range, applies the same ANSI/OSC and
  control normalization, and validates session, generation, offsets, SHA-256, and
  byte count. Runtime/terminal restart, removal, generation change, and capture
  eviction therefore fail closed instead of using the descriptor as content.
  Primary-root Git commit/diff pins use strict full-OID or fixed-scope references,
  re-run the filtered read-only query, and compare the bounded content hash plus the
  complete normalized source hash/byte count/truncation state. Worktree/staged drift
  fails stale, while commit-backed context remains valid only while its exact object
  is available. Duplicate/missing IDs, stale set identity, cross-session selection,
  and invalid image/child-handoff identities fail explicitly. Qt now
  loads project pins and covers authoritative file and editor-selection pin creation,
  CAS persistence, range metadata, per-turn inclusion, bounded order changes, and
  unpin in the Workbench render fixture. The Structure diagnostics surface also
  exposes explicit diagnostic pin/unpin: Qt rereads `workspace/diagnostics/raw`,
  validates project/root, media type, reference, SHA-256, and UTF-8 byte count, and
  submits only a metadata descriptor through CAS. Qt terminal context separately
  exposes transient selected text and persistent `固定最近输出`; the latter reads a
  bounded `terminal-excerpt/0.1` authority, validates it, and saves only the
  descriptor. The Git surface exposes explicit `固定差异` and `固定提交`; a real Git
  render fixture validates both authority reads, CAS persistence, labels, and unpin.
  Real Runtime and render fixtures cover assembly, metadata-only inspection,
  pin/unpin, mutable Git drift, and failure after source removal. The focused
  sidecar invalidation fixture persists `fresh` to `stale` transitions for
  root-scoped file/selection/diagnostic descriptors after workspace changes and
  terminal-excerpt descriptors after terminal lifecycle changes, then reopens the
  Runtime and verifies every stale marker remains durable. The Qt render flow
  consumes the sidecar-advanced identity and proves a stale terminal/selection pin
  can still be explicitly unpinned without weakening stale-CAS rejection. Remaining image/
  child-task production/lineage handoff, cross-resource atomicity, and complete cross-platform
  pin evidence remain incomplete; task 17.3 remains unchecked. The child-handoff
  fixture proves a parent-session-scoped text Artifact Blob is revalidated by
  owner, source-session, media type, UTF-8, hash, and byte count before assembly;
  inspection remains metadata-only; invalid source identity remains a bounded error.
  Qt workspace-watch and user-save callbacks mark loaded
  matching file, selection, and diagnostic pins stale locally; terminal
  restart/removal marks matching terminal-excerpt pins stale. None of these local
  indicators rewrite durable metadata; the sidecar reread remains authoritative.
  The command-output Artifact dialog now consumes Session/Item/reference-bound
  pages from `artifact/read-command-output-page` and exposes an explicit
  `固定完整输出` control only for the active project-bound Work session after
  client-side UTF-8/reference/SHA-256 plus canonical truncation-marker validation. The control assembles a
  session-owned metadata-only `artifact` descriptor (`metadata.item_id`, priority
  700, retained UTF-8 byte count) and submits it through the existing CAS path;
  it remains disabled for cross-session, recovery, deletion, reconciliation-blocked,
  busy, invalid-identity, or unsupported-media responses. Render coverage proves
  current-session enabled and cross-session disabled states. A real command
  Artifact click-to-persist fixture is still pending because the render runtime
  has no command producer; durable Artifact Blob reload and turn assembly remain
  covered by the protocol fixture. The legacy whole-artifact client API remains
  compatible but has no Workbench caller.
- `pinned-context-store/0.1` persists metadata-only sets using private immutable
  content-addressed objects and atomic project-pointer replacement. Store fixtures
  cover reopen/update, idempotency, absence of a content body field, retained old
  objects, object/pointer tampering, update refusal on damaged current authority,
  and symlinked-layout denial. Cross-resource event/object/Blob atomicity, Blob
  release/lifecycle binding, Windows execution, remaining image/child-handoff
  assembly, complete Git lifecycle, and their Qt
  surfaces remain open.
- Windows packaging, TLS runtime, scaling, IME, and accessibility evidence remain
  required before a Windows release claim.

## 2026-07-22 Partial Evidence

- Context-quality unit coverage now exercises large monorepos, ignored
  dependency/build/cache trees, irrelevant repository maps, nested instruction
  precedence, stale rereads, and intentional-exclusion versus budget-
  truncation semantics. These tests do not prove provider switching or
  cross-platform scale behavior.
- Provider error mapping has focused Rust and Codex stdio/AAP coverage plus
  Node syntax and deterministic gateway assertions. The gateway stream and
  existing gateway integration tests pass under the bundled application Node
  runtime. An earlier Homebrew Node gateway run was killed with exit 137, so that
  run was not used as gateway runtime evidence; the current Homebrew OpenSpec CLI
  validation completed successfully.
- `usage-authority/0.1` focused tests and the aegisy-agentd library suite pass.
  Codex usage Timeline items now carry the validated authority report and a
  stdio/restart fixture verifies it. Qt adds strict valid/malformed render
  fixtures and both changed C++ translation units pass direct syntax compilation;
  catalog pricing, cross-provider usage, billing, routing, and complete Qt
  runtime evidence remain absent, so task 20.2 stays incomplete.
- `context-threshold/0.1` has ten deterministic tests for authority,
  freshness, soft/hard limits, hysteresis, missing limits, and overflow. It is
  projected from provider-observed Codex usage into the durable Timeline and
  covered by the stdio/restart fixture. Complete bounded usage replay restores
  the latch after restart and distinguishes a genuinely empty usage history
  from replayed or uncertain evidence. The additive Session projection is
  consumed by Qt, whose deterministic threshold cache is capped at 128 entries.
  Compaction activation, provider compact, and automatic authority remain absent.
- `turn-trace/0.6` retains the existing Intent/completion-domain, final Provider-
  thread Usage authority, Codex Tool, and Runtime approval-policy contracts. Strict
  compatibility preserves hand-written `0.1` failed/completed JSON and the
  version-specific behavior and fixed identities of `0.1` through `0.5`;
  cross-version fields and future `0.7+` versions fail closed. The outer
  event remains `turn.trace.recorded/0.1`, SQLite remains v13, and no migration,
  backfill, or legacy event rewrite is introduced.
- The `0.3` contract/producer/Store verification scope covers deterministic
  `UsageAuthorityReport` identity and tamper rejection; exact schema, Provider
  source, observed time, report identity, and persisted Timeline Item binding; at
  most one Usage report; final-snapshot replacement without summing or assigning an
  Attempt/Retry; completed, failed, and interrupted retention before Error/Terminal;
  and no Usage event when no notification formed a valid authority report and was
  successfully persisted. Store fixtures cover Timeline Item first persistence,
  exact Session/Turn/kind/role/state, raw Provider snapshot/report/threshold
  reconstruction, final-valid-snapshot selection, malformed non-authoritative
  metadata, authority-downgrade rejection, idempotency, rollback, restart,
  direct-read rejection, and semantic-tamper quarantine.
- Threshold reconstruction now starts from the single Session `NoAction` state and
  replays the complete Session Item prefix through the target Turn's final Usage under
  the same 100,000-all-Item uncertainty bound as Runtime restoration. Later Turn
  Usage is excluded. Malformed authority-less Provider metadata advances the latch to
  conservative `PreviewRequired` consistently in the live producer, Store, and
  restart replay without becoming a Trace Usage event. Fixtures reject a forged first
  snapshot state and a hash-consistent cross-Turn 80%-90% hysteresis downgrade during
  admission, direct read, projection replay, and restart quarantine.
- The direct SQLite validator counts the complete Item prefix before reading Usage
  and consumes the Usage query lazily. It retains only one threshold latch, the
  current-Turn count, and the final valid report/Item binding, so peak memory is
  bounded by one Item rather than all historical Usage payloads. Repeated terminal
  admission and per-Trace projection replay still rescan bounded history; a verified
  prefix cache and single-pass replay remain required before large-Session performance
  is considered complete.
- Existing focused contract/producer tests plus Store coverage continue to cover
  source authority, Intent/domain applicability, exact completion-to-Intent
  binding, event ordering, duplicate usage, terminal evidence, secret rejection,
  atomic terminal persistence, idempotency, rollback, restart, and semantic-tamper
  quarantine. The pinned Codex Runtime now produces terminal-last traces for completed, failed,
  and interrupted Turns. Chat completion records three not-applicable domains;
  current read-only Work records no Workspace/Git change and keeps verification
  unknown. Completed is only a provider lifecycle terminal, not proof that a task
  changed files or passed tests. Stdio fixtures prove EOF transport failure,
  provider retry then failure, interruption, successful Chat recovery, a Work
  command/diagnostic completion, exact Session/Turn/environment/Intent binding,
  restart equality, excluded Context counts, content-free classification, and no
  fabricated Workspace/Git/Test evidence. Store admission rejects Work-to-Chat and
  Chat-to-Work Intent substitution before any terminal side effect, direct read
  rejects a hash-consistent mode substitution immediately, and projection replay
  quarantines it while retaining mode-less
  legacy `0.1` compatibility.
- `0.4` adds one content-free Tool pair for each observed Codex command lifecycle.
  Started records bind the closed provider status/source enums, provider timestamp,
  action, and stable identity from a fixed closed typed projection with
  `item_binding=not-persisted`; unknown Provider keys/values are excluded. Object-key
  order is canonical and action-array order is semantic. A completed, failed, or
  declined Tool is preflighted from Store's exact would-be sanitized persisted Item
  and becomes authoritative only after the Item/Artifact transaction commits. It
  binds a Session/Turn/Item domain-separated identity rather than the raw Provider
  Item ID, the complete sanitized payload SHA-256, output identity, duration, exit
  status, and terminal timestamp. Completed/nonzero, failed/zero,
  declined execution metadata, reverse time, and duration beyond the observed interval
  fail closed. Completed/cancelled Turns reject Started-only Tools; failed/interrupted
  Turns may retain Started without fabricating a terminal observation.
- The adapter compares a memory-only SHA-256 fingerprint over the complete original
  command/actions/cwd, including unknown fields, before display truncation. It is not
  serialized or persisted; neither it nor command/path/action content enters Trace.
  Tests cover long command/cwd suffixes, the 33rd action, opaque fields, object-key
  canonicalization, action-order sensitivity, secret redaction, and exact lifecycle
  drift.
  Store admission, direct read, projection replay, and restart reconstruct the Started
  and terminal observations from the exact command Item. Producer admission measures
  the complete outer durable envelope against exactly 72 KiB while reserving every
  open Tool terminal, worst legal failed/terminal metadata, and one emergency Started.
  Store independently rejects oversized Trace events at admission/read/replay/restart.
  Exact-budget exhaustion, a SQLite trigger failure, and Provider completion after an
  unmatched Started all produce a durable Started + Error + failed Terminal Trace and
  leave no terminal Tool, command Item, Blob reference, or disk object after restart.
- `0.5` adds exactly one Runtime approval-policy observation before Model/Context
  metadata. The pinned Codex producer binds the Runtime and `codex-app-server`
  adapter, Runtime version `0.144.5`, durable adapter version
  `codex-cli 0.144.5`, fixed producing Runtime identity `aegisy-agentd:0.1.0`,
  Provider-thread identity,
  configured/effective `approvalPolicy=never`, reviewer, read-only sandbox, and
  read-only permission profile. The observation is fixed to
  `decision_attribution=no-user-decision`, `user_decision_observed=false`, and
  `execution_authority=false`; it is Runtime policy evidence, not a user decision or
  permission grant.
- The Store independently rejects a Trace whose Runtime observation and policy
  observation were changed together, even when every inner/outer identity and Event
  hash is recomputed. The fixed producing Runtime identity preserves existing `0.5`
  replay after a future binary version change; any new producer version requires an
  explicit Trace contract revision or reviewed compatibility entry.
- `turn-trace/0.5` and `0.6` reject all `Approval` payloads before mutation until a durable
  approval-authority producer and exact ledger binding exist. Runtime policy,
  Runtime denial, and Provider Tool state therefore cannot fabricate an allowed,
  denied, or not-required user decision.
- Store admission, direct Trace read, projection replay, and startup quarantine
  independently require the durable Session Runtime binding, exact adapter and
  version, exact read-only permission profile, and a Provider-thread identity
  recomputed from the non-null durable backend thread. Seven focused `v0_5` tests
  and all 120 `workbench_store::tests` pass, including
  missing binding/thread and hash-consistent adapter, version, and backend-thread
  tampering across every read/replay/restart path.
- `0.6` adds a separate content-free `RuntimeDenial` payload for three active-Turn
  Codex request classes. Command-execution and file-change requests receive the fixed
  local response `{"decision":"decline"}`; permissions requests receive
  `{"permissions":{},"scope":"turn"}`, an empty grant interpreted as Runtime denial
  rather than a literal decline decision. Requests require a valid bounded request ID,
  exact active thread/Turn, bounded non-empty Item ID, and non-negative start time.
  Mismatched active-Turn requests receive `-32602`, fail the Turn, and produce no
  `RuntimeDenial` or `Approval`.
- The checked-in generated Codex `0.144.5` schema does not include these three
  `ServerRequest` definitions. No generated-schema validation is claimed for their
  shapes; current contract evidence comes from the same-version App Server protocol
  source and deterministic real-stdio fixtures, which must be re-reviewed on a pin
  upgrade.
- The adapter hashes the complete bounded Provider request message and persists only
  its identity. The durable request identity also binds `request_kind`, Trace,
  Provider-thread, and policy authority. Runtime first prepares a non-serializable
  ticket by checking identity, duplicates, the 128-denial ceiling, delivery ordinal,
  and the exact durable budget. The adapter then writes and flushes the fixed response;
  Runtime commits only after successful flush. A failed write or abandoned ticket
  produces no denial and remains on the fail-closed adapter/Turn error path. A
  reservation/preflight failure sends a fixed content-free JSON-RPC error and reuses
  the backend only after that error flush succeeds. Denial-response or fallback-error
  write failure marks the adapter unavailable/restart-required. Missing/invalid IDs
  and malformed params use a fixed `id:null`, `-32602` response and discard the
  backend, while a safely echoable binding mismatch may reuse it after a successful
  error flush.
  `decline-flushed` proves only the local child stdin write/flush, not Provider receipt
  or action. No prompt, Provider request/response body, path, command, output,
  credential, raw request ID, or raw Item ID enters the Trace.
- Every Runtime denial carries Runtime-observed metadata-only evidence and fixes
  runtime-policy attribution, user-decision, approval-authority, and execution-
  authority flags to false. It participates in Tool/Usage delivery ordering and must
  precede Error/Terminal. Terminal admission, direct read, projection replay, and
  startup quarantine recheck the exact durable Runtime/adapter/version/Provider-
  thread/policy binding, duplicate request identities, denial identity, evidence
  time/source, false-authority flags, and metadata-only redaction. Focused fixtures
  cover command, file, and permissions denials in order, sensitive request content
  absence, mismatched-Turn `-32602`, semantic tamper, and restart quarantine. Focused
  stdio recovery fixtures additionally cover budget preflight, invalid/malformed
  request rejection, response-pipe failure, backend reuse/discard state, and absence
  of a fabricated denial; their pass result is recorded only by the current gate run.
- Runtime denial, Provider `declined`, and genuine user Approval remain distinct.
  Provider `declined` is produced only as a terminal Tool observation, and
  `approvalPolicy=never` alone is only Runtime policy evidence. The read-only adapter
  has no genuine-user Approval producer. A future Approval producer must carry
  separate Approval-authority evidence and cannot derive a decision from either
  denial class or policy observation.
- The complete `0.6` gate passes 603 library tests with one ignored live fixture,
  10 threshold contract tests, 63 protocol tests, 19 stdio tests, formatting, strict
  Clippy, `git diff --check`, and strict OpenSpec validation. Complete genuine-user
  Approval/Change/Test production, non-command
  Tool families,
  authoritative per-Attempt/Retry Usage, and any AAP/Qt trace read,
  audit/export, or retention surface remain absent, so tasks 20.1 and 20.2 stay
  incomplete.
- At that threshold-cache stage, direct C++17 syntax checks passed for the Qt widget
  and render fixture, the render target completed MOC/RCC/compile/link, and its
  focused cache mode passed. The then-current full render run stopped at the model-
  profile read-only projection assertion; later complete desktop gates supersede
  that historical host result and now pass the full render fixture.

## 23.6 AAP And Adapter Contributor Documentation

- `docs/AAP-ADAPTER-CONTRIBUTOR-GUIDE.md` was added as the internal contributor
  source for schema ownership, stable/experimental versioning, Rust Runtime and
  Qt boundary rules, deterministic redacted fixtures, adapter pin/rollback, and
  release evidence.
- The guide references the checked-in AAP wire guide and Codex upgrade procedure,
  lists the local verification commands, and explicitly leaves Windows installer,
  named-pipe, ConPTY, and cross-platform evidence as external release gates.
- Verification: Markdown links and referenced paths were reviewed against the
  repository; `openspec validate build-aegisy-agent-workbench --strict` and
  `git diff --check` pass. No protocol or authority behavior is changed by this
  documentation task.

## 23.7 Troubleshooting Runbook

- `docs/Aegisy-TROUBLESHOOTING-RUNBOOK.md` records bounded diagnosis and safe
  recovery for sidecar startup, Workbench Store/reconciliation, adapter pin and
  crash loop, streaming response decode errors, Windows TLS initialization,
  terminal/Git/sandbox, and renderer restart failures.
- It preserves the existing security boundary: no secrets/raw provider bodies,
  no inferred process or mutation outcome, no TLS verification bypass, and no
  macOS claim for Windows packaging/runtime evidence.
- Verification: referenced docs and commands were reviewed against the current
  repository; `openspec validate build-aegisy-agent-workbench --strict` and
  `git diff --check` pass. This task changes documentation only.

## 23.8 Privacy And Diagnostic Export Documentation

- `docs/AEGISY-PRIVACY-AND-DIAGNOSTIC-EXPORT.md` is the internal privacy and
  support-export contract. It enumerates exact `local.*` and `cloud.*` data
  categories, default inclusion states, content opt-in scope, exclusion reasons,
  bounded redaction reports, deterministic `aegisy-diagnostic-bundle/0.1`
  envelope requirements, preview hash, removal controls, retention behavior,
  and support correlation-ID handling.
- The document explicitly excludes secure-storage credentials, prompts, source
  code, diffs, raw paths, terminal output, provider bodies, hidden reasoning,
  raw protocol frames, SQLite/WAL files, and automatic cloud upload by default.
  Redaction occurs before counting, hashing, previewing, serializing, or writing;
  stale previews, source loss, size overflow, and uncertain classification fail
  closed. It preserves local history and the current
  `file_mutation_authority=false`, `approval_recorded=false`, and
  `apply_available=false` boundary.
- Verification: the new document, all referenced existing docs, and the task
  entry were reviewed for internal consistency. This is a documentation-only
  slice; it does not claim that AAP/Qt diagnostic export or cloud upload has
  shipped. `openspec validate build-aegisy-agent-workbench --strict` and
  `git diff --check` are the required repository gates.

## 23.9 Support And Release Recovery Training

- `docs/AEGISY-SUPPORT-AND-RELEASE-RECOVERY-TRAINING.md` supplies the internal
  curriculum and sign-off checklist for support engineers, release owners, and
  incident commanders. It covers bounded first response, sidecar/handshake/
  heartbeat/reconnect, Store migration and reconciliation, Session Timeline
  retention/subscription, Windows `TLS initialization failed`, streaming decode
  errors, renderer restart, Git, terminal, and sandbox paths.
- The training red lines prohibit deleting repositories or Workbench history,
  fabricating success from process liveness or a closed socket, disabling TLS or
  permission validation, retrying ambiguous operations, and collecting secrets,
  raw provider bodies, prompts, code, diffs, paths, terminal output, PIDs, or
  raw stderr. It explicitly preserves the Agent/Codex read-only boundary.
- The guide states the separate roles of the troubleshooting runbook, privacy
  diagnostic export, and portable Session format; provides release-owner
  evidence fields and a clean-Windows evidence boundary; and defines seven
  disposable-fixture exercises with data-free sign-off records. Verification is
  documentation-only: this does not claim human attendance, a Windows runner
  pass, or a shipped exporter. `openspec validate build-aegisy-agent-workbench
  --strict` and `git diff --check` are the required repository gates.

## 3.6 Approval And File-Write Idempotency Foundations

- `agent-runtime/crates/aegisy-agentd/src/approval_ack.rs` defines the internal
  `approval-acknowledgement/0.1` metadata contract. Scope identity binds exact
  Session/Turn/scope; requirement identity binds that scope to the request
  fingerprint; operation identity additionally binds the idempotency key. The
  contract exposes no prompt, request body, command, path, content, or provider data.
- Equivalent requests and acknowledgement objects are exact idempotent replays.
  Binding drift or same-revision metadata drift is a stable conflict; every real
  transition advances one revision with non-decreasing time. Requested may move
  only to denied/expired/not-required, failed, or reconciliation-required evidence;
  terminal and uncertain states cannot be advanced by this producer.
- User decision, Approval, mutation, and execution authority fields are fixed false.
  Deserialization rejects unknown fields, unsafe integers/identifiers, common secret/
  token/JWT shapes, invalid deterministic identities, and nonexistent `allowed` or
  `approved` enum values. Eight focused tests, 773 `aegisy-agentd` library tests
  with one ignored live fixture, strict package/library Clippy, and formatting pass.
- This is not a genuine Approval producer and has no authority issuer. The
  schema-v21 Store is the historical draft-only baseline; the implemented v22 Store
  persists the complete typed source and metadata-only internal Session events. No
  production Approval path calls either slice, and there is no kind-specific outcome
  acknowledgement, outcome CAS/consume path, Public Timeline event, AAP capability
  or method, Qt review/freeze flow, genuine user decision, or authority.
  Runtime denial, Provider `declined`, and
  `approvalPolicy=never` must remain distinct from user Approval; task `3.6` stays
  unchecked.

- `agent-runtime/crates/aegisy-agentd/src/file_write_ack.rs` adds the internal,
  metadata-only `file-write-acknowledgement/0.1` contract. It binds the exact
  Session/project/root, idempotency key, request fingerprint, and Workspace Edit
  identity into a domain-separated operation identity; it rejects unknown fields,
  invalid identities, unsafe bounds, skipped revisions, backward timestamps,
  terminal evidence without an observation hash, and any authority flag. A
  `reconciliation-required` state cannot be turned into success by the producer.
- Focused coverage: five contract tests pass, including idempotent accepted
  replay, edit-identity conflict, lifecycle/revision drift, bounds, unknown
  fields, and fixed-false authority. The complete `aegisy-agentd` library test
  target passes 763 tests with one ignored live fixture; package Clippy with
  warnings denied and Rust formatting pass. This remains a contract foundation:
  the historical schema v21 stored its lossy draft and the implemented v22 slice stores the exact
  typed source while deriving that draft internally. No production producer,
  AAP/Qt route, outcome acknowledgement/consume or caller-CAS path, filesystem
  mutation, Public Timeline event, or authority exists.

## 22.5 Artifact Manifest Verification Foundation

- `include/artifact_manifest.h` and `src/artifact_manifest.cpp` define the
  bounded `aegisy-artifact-manifest/0.1` local verifier. Qt checks a present
  manifest before sidecar launch, validates exact runtime/adapter identities,
  versions, relative paths, canonical in-tree files, symlink/path escape,
  bounded manifest/artifact sizes, and streaming SHA-256 hashes. A failure
  suppresses launch and automatic reconnect; a missing manifest remains allowed
  only for developer-build compatibility.
- `artifact_manifest_verification` and `agent_runtime_environment` CTests pass.
- `artifact_manifest_runtime_startup` copies the real Release `aegisy-agentd`
  and a fixed `codex-cli 0.144.5` test adapter into a Unicode temporary directory,
  invokes the production generator, sets a bogus `AEGISY_CODEX_PATH`, and proves
  exact manifest-owned adapter selection through AAP initialize, initialized,
  shutdown, and clean daemon exit. The fixture declares the backend-required
  `runtime.codex-app-server` plus `permission.read-only` capabilities. Its bounded
  CTest timeout is 120 seconds for slow Windows antivirus/cold-start runners.
- `cmake/generate_artifact_manifest.cmake` deterministically generates the same
  contract from explicit final bundle files. It rejects missing, oversized,
  duplicate, symlinked, or out-of-bundle artifacts; output parents outside the
  canonical bundle; output paths that replace an artifact; invalid fixed IDs;
  and version/path metadata outside verifier bounds. It performs no network,
  process, environment-discovery, signing, or timestamp operation.
- `artifact_manifest_generation` generates the fixture twice, compares exact
  bytes/hashes, checks both artifact hashes and the pinned adapter version,
  rejects outside artifact and output paths, and invokes the production Qt
  verifier on the generated file. Together with `artifact_manifest_verification`,
  the focused CTest run passes 2/2.
- Rust Runtime now consumes the same adjacent manifest before Codex selection.
  A present manifest has priority over `AEGISY_CODEX_PATH`; exact Runtime and
  `codex-cli 0.144.5` versions, canonical paths, opened-file identities, and
  streaming SHA-256 values are checked. Duplicate or unknown JSON fields,
  non-portable paths, a Windows adapter path without an exact `.exe`, symlink/
  reparse-point components, any extra hard-link alias, path/content/file-identity
  drift, or a changed manifest identity within one startup attempt fail closed.
  Hashing uses a bounded 64 KiB heap buffer rather than a 1 MiB main-thread stack
  frame. Verification occurs immediately before both the `--version` probe and
  `app-server --stdio`, and a malformed present manifest never falls back to the
  developer override. Sixteen focused Rust tests and the complete Rust workspace
  pass with 1,078 tests and one explicitly ignored live Codex fixture, including
  the final 23/23 stdio/Codex target. Rust formatting and strict package Clippy pass
  on macOS. The complete desktop build and the focused
  `agent_runtime_environment`, `artifact_manifest_verification`,
  `artifact_manifest_runtime_startup`, `artifact_manifest_generation`, and
  `windows_packaging_policy` CTests pass 5/5 on macOS.
- The complete serial desktop gate passes 26/26. Strict OpenSpec validation and
  `git diff --check` pass.
- `include/update_signing_key_ring.h` and `src/update_signing_key_ring.cpp` add the
  bounded production trust authority for signed update metadata. A compile-time
  `aegisy-update-signing-trust-anchor/0.1` verifies generation-one bootstrap, and a
  prior validated authority verifies each exact `+1`
  `aegisy-update-signing-key-ring/0.1` rotation. The Ring retains every prior Key
  ID/public key, cannot widen prior validity or usage or reverse revocation, rejects
  lineage gaps/branches/cycles, and requires monotonic Ring signing time. Authority
  state binds each key's first admitted generation/time; a Ring or Artifact Set
  signature before admission fails. Since `signed_at_ms` is signer-controlled,
  first-time bootstrap/rotation requires the signer to be active both at the declared
  time and at local verification time. An already accepted exact envelope may replay
  idempotently after expiry, but offline expired-signer recovery requires a future
  independent witness/checkpoint. Explicit revocation still blocks use. Generation
  one preserves its fixed authority identity, while later generations bind the
  complete admission history even when different histories converge on one later
  Ring envelope.
- Production Artifact Set verification is now
  `aegisy-update-artifact-set/0.2`. Its 256 KiB envelope uses the generated lossless
  Transport JSON parser and rejects duplicate decoded keys, invalid UTF-8/surrogates,
  unsafe numbers, excessive depth/nodes, unknown fields, unsafe canonical HTTPS
  paths, noncanonical Base64, and unsupported platform/installer combinations. The
  fixed ordered payload binds release/channel, Key ID, signed/exclusive-expiry time,
  payload identity, target application version/platform/architecture/size/SHA-256,
  full-installer URL/name/size/SHA-256/Sparkle signature, target manifest plus exact
  Runtime/adapter identity/version, and one to 64 strictly increasing compatible
  sources with complete application size/SHA-256 and Manifest identity. Candidate
  signatures require a currently active Artifact Set key; historical installed
  receipts require validity at their signed time but remain subject to latest-Ring
  revocation and validity cutoffs.
- Production compatibility no longer accepts a publicly constructible installed
  tuple or caller-selected verification paths. `verifyCurrentInstallationAuthority`
  requires the executing image to be the fixed Windows `AegisyClient.exe` layout or
  macOS `AegisyClient.app/Contents/MacOS/AegisyClient` layout. It derives the exact
  adjacent receipt, Manifest, and Runtime paths; verifies the signed receipt target
  plus complete Manifest/Runtime/adapter bytes, identity, version, ordinary-file/link
  policy; and derives an opaque authority whose identity binds the receipt, installed
  tuple, trust anchor, Ring generation/identity/authority, receipt signer, canonical
  directory/file identities, and the current application size/SHA-256. It reobserves
  the application and directories after
  artifact verification. Each candidate evaluation rederives and rereads that graph
  and rejects drift before compatibility is evaluated. A post-stage review found that
  Qt canonical Windows paths use `/` while the two containment checks appended native
  `\\`, which would reject a valid installed layout. Both production consumers now
  use one explicit path-flavor helper: Windows normalizes `/` and `\\`, compares
  case-insensitively, handles drive and UNC roots, and requires a strict
  separator-delimited descendant; Posix remains case-sensitive. A platform-neutral
  fixture covers equal, sibling-prefix, cross-drive, root, separator, case, and UNC
  strings, while `windows_packaging_policy` requires both source consumers to retain
  the shared helper. This closes the source regression but is not clean Windows
  execution evidence. The scalar tuple and
  arbitrary-root, raw-key, and legacy helpers are available only to the CTest target
  through `AEGISY_UPDATE_ARTIFACT_SET_TESTING` and
  `AEGISY_UPDATE_SIGNING_KEY_RING_TESTING`. Compatibility still requires the selected
  channel and release sequence above both the installed sequence and caller-supplied
  accepted high-water value. The returned evaluation identity binds those inputs,
  evaluation time, candidate identity and signer, trust-anchor/Ring authority, and
  installed authority.
  Compatible and incompatible results both keep `downloadAuthorized` and
  `installAuthorized` false. The focused `update_artifact_set_compatibility` CTest
  passes with macOS and Windows positives, the exact canonical payload/identities, an
  externally generated fixed Ed25519 vector, signature and nested field tampering,
  installed tuple matching/mismatch, source ordering/count bounds including
  a valid 64-source set, URL encoding/dot/device-name rejection, BOM/invalid UTF-8/
  escaped duplicate/lone-surrogate/depth/node rejection, exact 256 KiB, installer
  size, JSON-safe integer, clock-skew, key/signature, replay, and false-authority
  boundaries. New authority fixtures cover missing authority, bounded/oversized public
  keys, signed-receipt tamper and wrong key, application/channel/platform expectation
  drift, a valid signed receipt replacement with a changed release sequence, signed
  receipt Runtime/adapter version disagreement, Manifest byte replacement, Runtime/
  adapter content drift, separated receipt/Manifest directories, and revalidation of
  a cached authority. It also replaces the application image after authority creation
  and runs a copied test image through the public factory in a Unicode, production-
  shaped `AegisyClient` layout. The shared Manifest fixture rejects duplicate decoded
  JSON keys, invalid UTF-8/BOM/surrogates, unsafe numbers/depth, in-bundle Runtime and
  adapter links, linked parent components, and extra hard links; the Manifest and
  installed receipt themselves must also be single-link ordinary files.
  A separate `AegisyUpdateArtifactSetProductionFixture` target compiles without either
  testing macro and with a fixed fixture Root. The parent copies that binary into the
  real macOS/Windows layout, then proves embedded Root bootstrap, generation-two
  rotation, a historical Root-signed installed receipt, a rotated-Key candidate, and
  production rejection of a validly signed legacy `0.1` envelope. Both decisions keep
  download/install authority false. Focused Key Ring fixtures additionally cover
  first-time bootstrap/rotation denial after signer expiry, exact accepted-envelope
  replay, admission-time backdating, converged-Ring admission-history separation,
  Ring signed-time rollback, revocation, validity narrowing, lineage, identity,
  bounds, and signature failures.
  `windows_packaging_policy` requires this CTest and the real manifest startup fixture
  in the complete release test graph.
- The local `aegisy-update-signing-key-ring-continuity/0.1` cache persists only
  exact signed envelope bytes and bounded integrity metadata: immutable generation
  objects, a private bootstrap marker, and an atomically replaced head. It never
  persists or deserializes `Authority`, `accepted_at`, admission time, or verification
  tickets. Every load replays generations `1..N` from the embedded Root at the
  current `nowMs`; strict current-time replay is `Authoritative`, while only a
  complete chain whose current-time failure is one of the explicitly replayable
  activity errors (`bootstrap-root-invalid`, `signer-inactive`, or
  `no-current-active-usage`) is `CachedButNotAuthoritative` with no valid
  Authority; revoked, malformed, structurally invalid, and signature-invalid
  chains remain `Invalid`. Limits are 64
  envelopes, 128 KiB per envelope, and 8 MiB per chain. Marker/head/object
  identity, exact envelope binding, prefix-head continuity, unknown/partial
  deletion, Unix permissions, links, local lock, expected-head CAS, exact no-write
  retries, and stale/tampered evidence are covered by
  `update_signing_key_ring_cache_integrity`. Complete deletion means the cache
  directory is absent and is `Empty`; a retained directory with partial evidence is
  `Invalid`, not `Empty` or detected deletion. The cache is not connected to an
  updater, network, download,
  install, rollback, resume, execution, anti-rollback, anti-deletion, trusted-time,
  or expired-signer-recovery authority; this is macOS/local evidence only.
- `include/update_progress_record.h` and `src/update_progress_record.cpp` add the
  internal `aegisy-update-progress-record/0.1` continuity Store. Its exact current
  record binds release sequence, artifact-set identity, ordered phase, revision,
  monotonic update time, previous identity, and current identity. It admits only
  candidate-evaluated -> download-started -> download-verified -> install-started ->
  installation-observed, allows an exact uncertain retry, blocks same-sequence
  identity conflict and replacing an incomplete release, and uses bounded lossless
  JSON, single-link files, private Unix permissions that reject owner-execute,
  group/other, and setuid/setgid/sticky bits, `QSaveFile`, and post-commit reread.
  `update_progress_record_integrity` covers transition/retry/conflict,
  rollback/deletion against an external anchor, duplicate-key/tamper, forged
  authority flags, over-permissive Unix modes, symlink, and hard-link cases.
  Download/install/rollback authority is fixed false in every API and persisted
  record.
- This Store is continuity evidence, not the required anti-deletion authority. A
  trusted caller must retain the expected identity/floor elsewhere; deleting both
  permits a fresh record. `QLockFile` provides a local single-writer gate, but the
  Store has no secure-storage anchor, cross-resource CAS, crash-injection/framework
  compensation, `UpdateManager` integration, or signed package identity. The local
  lock fixture passes and the Windows release-policy test keeps the focused target in
  the complete clean-runner graph, but no clean Windows or process-crash execution
  result exists yet.
- Current macOS and Windows scripts do not bundle a pinned Codex adapter and do
  not invoke the generator. Packaged Runtime has no compile/package identity that
  makes manifest absence non-downgradable, and path verification followed by
  path-based process creation retains a replacement window that needs a reviewed
  handle/platform-signature and install-permission boundary. Updater compatibility,
  signed package evidence, and clean Windows execution remain absent. The clean
  Windows Unicode workflow's unfiltered CTest is configured to execute the new
  real-daemon fixture, but source/workflow inspection is not Windows execution. The
  local macOS-to-Windows Cargo check stops earlier in SQLite/Tree-sitter C because
  Windows SDK headers are unavailable, so it supplies no Windows result. The signed
  compatibility evaluator is not called by either `UpdateManager`. Its fixed-layout
  factory now binds canonical path, native file identity, size, and SHA-256 for the
  current application-path target, receipt, Manifest, Runtime, and adapter. Every hash
  read matches the actual opened handle identity to both before/after path observations;
  Runtime and adapter must be distinct canonical paths and native files. Cached
  authority rejects exact-byte replacement of each of the five files. This still
  observes the current application path rather than proving the image loaded at process
  start. Artifact Set `0.2` binds the application size/SHA-256, but the factory does
  not verify macOS code signing/notarization, Windows Authenticode, or the outer installer;
  it therefore is not signed-package membership authority. Key IDs, validity,
  revocation, sequential rotation, and admission history are locally enforced. A
  local envelope continuity cache now retains exact signed Ring bytes and bounded
  integrity metadata, and reconstructs verification by replaying `1..N` from the
  embedded Root at current time; it is not an authenticated publication or secure
  high-water anchor and supplies no cross-restart anti-rollback authority. The
  release Root defaults empty and no authenticated Ring fetch exists. The verified read
  handles are not retained through later process or
  install actions, so those cross-file TOCTOU windows remain. The scalar high-water and
  new local progress file have no trusted
  anti-deletion anchor or cross-resource updater/secure-anchor transaction.
  Production recovery must bind
  their sequence, artifact-set identity, phase, and current record identity to secure
  durable authority before an exact retry can resume. Sparkle candidate
  veto does not cover resumed updates, the current contract binds no generated delta,
  and WinSparkle 0.9.3 has no pre-download candidate veto. Server feed filtering or
  post-download callbacks cannot replace that local gate. Task `22.5` remains
  unchecked.
- After the Key Ring/Artifact Set `0.2` stage, the complete application build and all
  `29/29` serial desktop CTests pass in 121.72 seconds. An independent read-only
  security review finds no remaining P1/P2 defect in dual-time admission,
  exact-envelope replay, generation-two admission-history binding, or production API
  isolation. `nm -gU -C` on the application and no-testing-macro fixture exposes the
  Authority-based Artifact Set `0.2` and Key Ring bootstrap/rotation entry points,
  with no testing namespace, raw-key, or arbitrary-root helper. Strict OpenSpec
  validation reports the change valid and `git diff --check` passes. This supplies
  macOS/local contract evidence only; it is not signed-package, real updater,
  zero-byte incompatible-download, resume/install recheck, persistent
  Ring/anti-rollback, or Windows execution evidence.
- After correcting the Windows canonical-containment regression, the complete
  application build and the focused `artifact_manifest_verification`,
  `artifact_manifest_runtime_startup`, `artifact_manifest_generation`,
  `update_artifact_set_compatibility`, and `windows_packaging_policy` CTests pass
  `5/5`. The complete serial desktop gate passes `29/29` in 114.43 seconds. Both
  production consumers contain no `QDir::separator()` call, strict OpenSpec
  validation returns valid with exit code zero, and `git diff --check` passes. The
  PostHog DNS warning occurs only after the successful validation result and is not
  Windows execution evidence.

## 22.6 Emergency Workbench Disable Foundation

- Qt accepts the policy only from the authenticated HTTPS account origin, rejects
  redirects, non-JSON and oversized responses, and emits fixed content-free failure
  codes. The inner envelope has an exact field set and Ed25519 signature under the
  pinned updater key; no Session, prompt, path, model body, credential, or arbitrary
  server message can enter the policy cache or banner.
- Policy tests cover valid disable, tamper, unknown content fields, expiry, lifetime,
  persistence, lower-sequence rollback, same-sequence conflict, higher-sequence
  recovery, cache-envelope rollback after restart, missing envelope, missing marker,
  and first-open absence. The marker advances before the envelope so an interrupted
  replacement is fail-closed.
- Qt blocks new Workbench requests immediately and switches the Sidecar generation
  in both disable and recovery directions. The focused fake-sidecar test observes
  exact `normal -> emergency -> normal` environments and `-32153` for a new Session.
  Rust starts the emergency Store without Codex, filters capabilities, rejects new
  Session/Turn/workspace/import/restart mutations centrally, reopens existing local
  history, and completes a real portable preview/export. Missing data-root and
  Store-open failure paths use an unavailable emergency backend and never fall back
  to Codex.
- Store corruption/recovery retains the emergency flag and central `-32153` mutation
  gate while exposing only recovery diagnostics. The Qt and Rust request allowlists
  match except for Rust's handshake-only `initialized` notification. Unreachable
  retention-policy read is excluded because the current protocol exposes only the
  broader manage capability.
- Disable intentionally retires the normal Sidecar even when it owns in-memory work,
  using bounded graceful shutdown and kill escalation. Pending or active work is
  never inferred successful. Keeping that generation for cleanup would leave the
  Runtime in normal mode and reduce enforcement to Qt only; same-process cleanup plus
  atomic queued-mutation revocation therefore remains a future protocol design, not
  a shortcut in this foundation.
- Focused evidence passes `workbench_emergency_policy`,
  `api_client_account_and_keys`, `agent_runtime_environment`, and the three Rust
  emergency tests. The environment fixture verifies the Qt mutation gate immediately
  after policy application, before the emergency generation becomes ready. Corrupt-
  Store recovery preserves the emergency flag, required recovery capability marker,
  diagnostics, and central `-32153` gate without starting Codex. The final combined
  gate passes 44 AAP tests, 774 Sidecar library
  tests with one ignored live fixture, 7 daemon tests, 10 threshold tests, 21 Runtime
  tests, 23 Schema tests, 68 protocol tests, and 23 stdio/Codex tests; strict Clippy
  and Rust formatting; the complete desktop build and all 19 CTests; strict OpenSpec
  validation; and `git diff --check`.
- This is not production completion. The server route and signing publisher are not
  deployed, first install without a policy intentionally preserves current behavior,
  QSettings does not provide OS-secure anti-deletion anchoring if both cache halves
  are removed, a healthy-Store diagnostic bundle exporter is absent, and signed
  macOS/Windows end-to-end disable/recovery evidence has not run. Keep `22.6`
  unchecked.

## 23.10 Visible-State Review Foundation

- `docs/AEGISY-WORKBENCH-VISIBLE-STATE-MATRIX.md` is the maintained inventory for
  empty, loading, offline, permission, conflict, failure, interrupted, and recovery
  states that currently exist in the Qt Workbench. Each row distinguishes the
  visible variants, stable locator, exact executable evidence, and residual gaps.
- Stable locators now cover the empty Timeline, project list, file status, and
  status/failure notice categories. Notice assertions compare the before/after
  instance count, newest child, exact text, and semantic severity so an earlier
  notice cannot satisfy a later transition.
- The render fixture proves the initial empty surface; fail-closed capability
  loading; the read-only permission boundary; an external-file conflict with Save
  disabled and external CRLF bytes preserved; stable failure-notice locators, exact
  text, and semantic severity; cancellation
  acknowledgement remaining non-terminal; authoritative interruption restoring
  Send with a visible terminal notice; and synthetic offline/reconnected Qt status
  projections. The synthetic connection signal is UI evidence only, not proof of a
  transport disconnect or reconnect barrier.
- Focused evidence passes the `AegisyAgentWorkbenchRenderTest` build and
  `agent_workbench_render` CTest (1/1). Loading variants for search/history/terminal
  attachment, real Monaco/xterm crash fallback, complete secondary-dialog coverage,
  accessibility, focus order, screen readers, Chinese IME, high contrast, display
  scaling, and clean Windows execution remain unverified. Keep `23.10` unchecked.

# 1.4 Third-Party Component And License Inventory

- `docs/THIRD-PARTY-COMPONENT-INVENTORY.md` inventories the exact repository
  versions and status of Codex, AAP/ACP, Monaco, xterm.js/FitAddon, esbuild,
  Tree-sitter and all selected grammars, external language servers, PTY/process,
  cryptography/storage, updater, icon, Qt, OpenSSL, and future sandbox components.
- Version evidence comes from the Codex source pin and npm package metadata,
  `workbench-web/package-lock.json`, `agent-runtime/Cargo.lock` plus Cargo package
  metadata, the pinned Sparkle/WinSparkle CMake files, and the checked-in Lucide
  license. The document separates bundled, external, and not-yet-selected
  components and records their release obligations without treating the inventory
  as legal approval.
- OpenSpec `1.5` still owns legal review, while `22.4` owns complete signed-package
  NOTICE/license inclusion. A developer installation, successful build, or upstream
  license identifier is not accepted as proof that the shipped bundle is complete.
  `openspec validate build-aegisy-agent-workbench --strict` and
  `git diff --check` are the repository gates for this documentation stage.

## 1.6 Milestone 0 Performance Budgets

- `docs/AEGISY-MILESTONE-0-PERFORMANCE-BUDGETS.md` predeclares reference
  macOS/Windows machine classes, signed Release builds, 20 measured runs, median/
  p95/maximum/failure reporting, cold/warm definitions, monotonic clocks, complete
  process-tree resource accounting, and one bounded repository/Timeline/editor/
  terminal fixture.
- Absolute p95 thresholds cover installer and installed growth, legacy and Workbench
  startup/readiness, idle/active memory, editor open/input/save, PTY echo/throughput,
  initial/incremental indexing CPU and time, and renderer/sidecar/full-app recovery.
  Correctness gates separately forbid legacy blocking, orphan processes, state loss/
  fabrication, weakened security, unsafe indexing, and content-bearing evidence.
- This task defines budgets only. OpenSpec `2.7` remains unchecked until signed
  packages are measured on both reference platforms. Missing metrics fail rather
  than defaulting to zero, and a p95 miss blocks the UI technology ADR unless a
  product/security owner records a time-bounded non-correctness exception.
  `openspec validate build-aegisy-agent-workbench --strict` and
  `git diff --check` are the repository gates for this documentation stage.

## 4.1 Rust Workspace, Dependency Audit, And Release Profile

- `agent-runtime/Cargo.toml` owns the `aegisy-aap` and `aegisy-agentd` workspace,
  resolver v2, committed lockfile, exact direct dependency pins where required, and
  the reviewed Release profile (`lto = "thin"`, `codegen-units = 1`, `strip = true`).
  The local path dependency also declares its exact workspace package version, so a
  wildcard requirement cannot bypass the dependency policy.
- `deny.toml` and `scripts/verify-rust-dependencies.sh` pin the executable contract
  to `cargo-deny 0.19.9`, require locked Cargo metadata, and run all four checks.
  Advisories use the RustSec database and fail if it cannot be fetched/opened;
  licenses are restricted to the reviewed SPDX identifiers present in the lockfile;
  sources are restricted to crates.io with no Git allowlist; yanked and wildcard
  dependencies are denied. Existing transitive duplicate version families remain
  explicit warnings rather than hidden skip entries.
- `.github/workflows/rust-quality.yml` runs formatting, locked workspace tests,
  locked all-target Clippy with warnings denied, locked Release build, and the pinned
  dependency audit for Rust changes. The Windows packaging workflow runs the same
  locked quality/Release/audit gates before installer construction.
- CMake selects Cargo `target/release` and passes `--release` for Release desktop
  builds, including multi-configuration generators; Debug developer builds retain
  `target/debug`. An isolated macOS Release configure/build produced an arm64
  `agent-runtime/target/release/aegisy-agentd` and copied the byte-identical binary
  into `AegisyClient.app/Contents/MacOS`.
- Local verification passes the dependency audit (`advisories`, `bans`, `licenses`,
  and `sources`), Rust formatting, strict Clippy, the locked Release workspace build,
  and the complete locked Rust workspace suite: 1050 passed, zero failed, and one
  explicitly ignored installed-Codex live fixture. The isolated Release desktop
  build and serial aggregate CTest run pass all 23/23 targets, including
  `agent_runtime_environment`. This was the `4.1` workspace/release evidence; the
  next section records completed macOS peer validation, while `4.3` and `4.4` still
  own Windows named-pipe and bootstrap-authentication evidence.

## 4.2 macOS Owner-Only Unix Socket And Peer Validation

- The explicitly selected Qt socket mode launches the same bundled sidecar with a
  fresh private endpoint path and never falls back to stdio after endpoint, peer,
  handshake, or disconnect failure. Stdio remains the default until bootstrap
  authentication is implemented.
- Rust opens the private parent and endpoint directories with no-follow descriptor
  operations, rejects extended ACLs and existing or drifting objects, creates exact
  `0700`/`0600` permissions, bounds accept time, and checks that `getppid()` remains
  the expected Qt parent before accept, after accept, and after same-UID/exact-PID
  peer verification. The accepted stream is restored to blocking mode before the
  existing bounded reader is used.
- Qt independently verifies the same UID and exact `QProcess` PID, records a
  generation-scoped peer proof, and refuses socket ingress, writes, or initialize
  without it. Endpoint-invalid, peer-mismatch, and unexpected-disconnect paths use
  generation-owned terminate/kill/reap handling and do not replace a specific
  security failure with a generic disconnect message.
- Rust and Qt bind cleanup to device/inode/UID identities. Random quarantine closes
  check/use races; replacement objects remain untouched. If process termination
  interrupts the sidecar after it quarantines its own socket, Qt removes only the
  exact recorded socket identity before removing the exact endpoint directory.
- Generated Rust, TypeScript, and Qt/C++ transport-security types use a strict union.
  Real Schema validation accepts only truthful stdio or verified Unix request/result
  facts and rejects Unix `peer_verified: false`, Unix `authenticated: true`, stdio
  `peer_verified: true`, and unknown transports. The verified socket still reports
  `authenticated: false` and grants no additional authority.
- Local macOS verification passed:
  - `npm --prefix agent-runtime/aap-schema run generate:check` including all 47
    package files.
  - Rust formatting, locked workspace tests (`1062 passed`, zero failed, one
    explicitly ignored installed-Codex live fixture), all-target strict Clippy, and
    locked Thin-LTO Release build.
  - Focused Rust macOS socket tests (`11/11`) and handshake Schema tests (`24/24`).
  - Real Qt-to-Rust `agent_runtime_macos_socket`, including successful initialization,
    exact security facts, wrong-PID rejection, forced process cleanup, and endpoint
    cleanup; the binary passed 20 consecutive repetitions.
  - Complete Debug desktop build and all `24/24` CTests, strict OpenSpec validation,
    and `git diff --check`.
- This is local macOS evidence, not signed-package evidence. Windows named-pipe
  ACL/peer validation remains `4.3`; one-time bootstrap authentication remains
  `4.4`; `authenticated` MUST stay false until that proof exists.

## 4.3 Windows Named-Pipe Qt Attempt Isolation (Partial Evidence)

- Qt creates one `QLocalSocket` per Unix-socket or Windows named-pipe connection
  attempt. Ready-read, connected, error, and disconnected handlers capture a guarded
  socket pointer, process generation, and monotonic attempt epoch and validate all
  three before any state change or input read.
- Peer verification is bound to both the process generation and attempt epoch.
  Initialize, socket ingress, and writes require that exact proof. Retiring a socket
  clears the proof, disconnects callbacks, aborts the endpoint, and uses deferred
  deletion, so old callbacks cannot clear a newer proof, schedule reconnect,
  terminate a newer process, or initialize a newer connection.
- The platform-neutral Qt fixture drives all four signal classes from an old process
  generation and an old attempt, verifies actual stale bytes remain unread, and runs
  queued signals through retirement and deferred deletion before confirming the new
  connection state is unchanged. The complete desktop build, focused Runtime
  environment and real macOS socket tests, and all `25/25` serial macOS CTests pass;
  the focused Runtime environment target also passes ten consecutive repetitions.
- The dedicated Windows E2E source now retains both launch endpoint names, requires
  rotation across process generations, proves the retired endpoint cannot connect
  while the replacement remains ready, and exercises remote-form access only after
  a control pipe with the same protected current-token-user DACL but without
  `PIPE_REJECT_REMOTE_CLIENTS` proves that the host route is available. A local
  connection must still succeed after the rejected remote-form
  attempt. A supervised fake sidecar plus a parent-owned `QLocalServer` also proves
  Qt observes `named-pipe-peer-mismatch`, sends no initialize bytes, does not fall
  back to stdio, suppresses reconnect, and reaps the exact fake process generation.
- Independent review of commit `0f36ae1` found that the Windows-only fixture could
  lose the fake executable path through the ANSI environment boundary in the required
  Unicode checkout, expected six-second process reconnect exhaustion where production
  actually performs same-generation endpoint retries until its 60-second startup
  deadline, accepted any remote-form error as security evidence, and could release an
  `OVERLAPPED` before cancellation completed. The corrected source sets the path with
  `SetEnvironmentVariableW` and asserts the exact selected executable, shortens only
  the active test timer while requiring multiple endpoint attempts and no process
  reconnect, requires `ERROR_ACCESS_DENIED` and independently observes
  `PIPE_REJECT_REMOTE_CLIENTS`, and drains exact cancellation through
  `CancelIoEx`/`GetOverlappedResult` before closing handles. The complete local build
  plus focused Runtime environment, macOS socket, and Windows packaging-policy tests
  pass after this correction. A workflow run for `0f36ae1` is not completion evidence;
  the corrected commit must run again.
- This remains partial implementation evidence. It does not replace a clean Windows
  run proving wrong-server-PID rejection, remote-form client rejection, real named-
  pipe old-endpoint/callback ordering, or the complete dedicated Windows E2E. Keep
  task `4.3` unchecked.
- The read-only Windows validation job now performs its complete Release build and
  unfiltered CTest from the clean `windows-验证-源码` checkout, so the Qt
  environment fixture and dedicated named-pipe E2E will execute in the same checkout
  and validation run as the generated-protocol and desktop gates. The workflow has
  not yet completed after this expansion. The new remote-form, Qt wrong-server-PID,
  and old-endpoint assertions have not compiled or executed on Windows;
  PID/creation-time mismatch already has a deterministic Rust fixture but likewise
  still needs Windows-runner execution. Focused local
  `agent_runtime_environment` and `windows_packaging_policy` CTests pass, but neither
  compiles the Windows-only E2E branch. Do not infer Windows results from source or
  workflow configuration.

## 2026-08-07 Cross-Platform CI Gate Repair

- Windows validation run `31152998711` used the clean Unicode checkout and passed
  the complete fail-closed Rust gate, including formatting, locked workspace tests,
  strict all-target Clippy, Release build, offline package, and dependency audit.
  The job then reached its exact 90-minute timeout while the combined Qt Release
  build and unfiltered CTest step was active. That step is `cancelled`, not passed;
  installer construction, package verification, artifact upload, and publish were
  skipped. The validate timeout is now 150 minutes, and
  `windows_packaging_policy_test.cmake` contains a negative mutation proving that a
  return to 90 minutes is rejected.
- Focused Windows run `31154013436` at current product/test code passed the real
  Release `agent_runtime_windows_named_pipe` E2E. It is valid focused evidence for
  the repaired named-pipe restart generation and endpoint rotation, but its own
  workflow declares that it is outside the release validation matrix. The Monaco
  probe in that run did not propagate a nonzero process exit, so no Monaco success
  is inferred. The probe now enables fail-closed native PowerShell semantics,
  throws on a nonzero child exit, and has positive plus negative policy fixtures.
  Diagnostic `QLocalSocket` and `WaitNamedPipeW` connections were removed from the
  named-pipe failure path so observation cannot consume or occupy the endpoint under
  test; passive process, endpoint, attempt, failure, and diagnostic output remains.
- macOS run `31154005122` exited from CTest after the workflow built only the
  `AegisyClient` target. Since independently registered test executables are not
  dependencies of that target, this is not a complete macOS product-test result.
  The workflow now builds the complete default graph and invokes one unfiltered
  CTest command with `--no-tests=error`. `macos_ci_policy_test.cmake` is CRLF-safe
  and rejects both target-only builds and filtered CTest.
- Local Windows-host verification passes all three workflow policy fixtures,
  their embedded negative cases, and `git diff --check`. This host has no usable
  Rust/MSVC/Qt 6 toolchain, so it cannot replace the next clean macOS and Windows
  runs. Keep `3.10`, `4.3`, `4.4`, `14.2`, and `14.9` unchecked.

## 2026-08-10 Linux Stub And Windows ConPTY ANSI CI Repair

- Rust Quality run `31325268705` (`ubuntu-24.04`, main `a61ce4f`) passed checkout,
  toolchain setup, and formatting, then failed `Run locked unit tests` with exit 101.
  Clippy, Release, and dependency-audit steps were skipped. The job exposed nine
  Linux-only `E0609` errors: the unsupported terminal module's snapshot retained
  only `terminal_id`, while shared Runtime response code consumed the same 21 fields
  as the macOS and Windows implementations. The stub now has the exact shared field
  set. A non-macOS/non-Windows protocol branch sends both an invalid request and a
  valid `24x80` request and requires `-32090`, proving the shape repair does not
  fabricate a Linux terminal or bypass the platform gate.
- Windows validation run `31325268703` (`windows-2022`, the same commit) passed the
  clean Unicode checkout and release-version-reuse gate, then failed
  `Verify Windows agent runtime` with exit 1. Qt/OpenSSL installation, the desktop
  build, installer construction, package verification, upload, and publish were all
  skipped. The real ConPTY capture after interrupt was
  `ESC[31mAEGISY_ANSI_AFTER_INTERRUPT CR ESC[m LF`; the old test required the exact
  no-CR `ESC[0m` form. The assertion now uses a platform-neutral helper that requires
  an immediately adjacent, syntactically valid non-reset SGR before the exact marker,
  allows only one optional CR, and requires `ESC[m` or `ESC[0m` immediately after it.
  Negative cases reject a bare marker, ANSI elsewhere, reset-only prefix, non-reset
  suffix, and repeated CR.
- macOS run `31325268727` at the same commit passed configure/build and failed
  `Run Tests` with CTest exit 8. Its public annotation does not name the failed test.
  The Linux/Windows corrections do not claim to repair that run; a new macOS result
  remains part of the cross-platform gate.
- Local code evidence passes `cargo fmt --all --check`, strict workspace all-target
  Clippy, the complete workspace test run (`1137` passed, one ignored), the desktop
  build, all `32/32` serial CTests, and `git diff --check`. The helper's focused
  positive/negative tests pass `2/2`, and independent review found no additional
  P1/P2 source-logic defect. This is not clean-runner evidence. No new run contains
  the fix yet;
  keep `3.5`, `3.10`, `4.3`, `4.4`, `14.2`, and `14.9` unchecked and keep
  Agent/Codex read-only.

## 2026-08-10 Codex Startup, Windows Clippy, And Timeline Disconnect CI Follow-Up

- Ubuntu Rust Quality run `31328260123` failed
  `stdio_codex_startup_crash_loop_is_bounded_and_unavailable`: the child exited
  before the initialize write, but direct `serde_json::to_writer` into child stdin
  labelled the resulting `BrokenPipe` as an encoding error, so startup stopped after
  one attempt instead of the required three. Codex messages are now serialized to a
  byte buffer before one write/flush path. A deterministic BrokenPipe writer proves
  the transport classification and exactly three bounded attempts without weakening
  version/protocol non-retry behavior.
- Windows validation run `31328260149` stopped in strict all-target Clippy with
  `large_enum_variant` for `WorkbenchStoreOpen`; Qt installation, desktop CTest, and
  packaging never ran. The writable variant now temporarily owns
  `Box<WorkbenchStore>`, and the only two production consumers immediately unbox it.
  Read-only recovery behavior and Store ownership are unchanged.
- macOS run `31328260142` failed only `agent_runtime_environment` because the
  `timeline-sync-disconnect` fake exited every automatically reconnected Runtime
  generation while the assertion counted Sync globally. The fixture now exits only
  generation one and holds reconnect behind a signal-blocked test timer. It
  requires the exact pending request to fail once, directly verifies that ID is
  removed and retired plus the replay capability is cleared, then explicitly starts
  generation two and proves each generation owns exactly one expected Sync. The
  focused CTest and `--repeat until-fail:20` pass locally.
- Local gates pass the locked complete Rust workspace (`1138` passed, one explicitly
  ignored installed-Codex live fixture), Rust formatting, strict all-target Clippy,
  locked Release workspace build, complete desktop build, all `32/32` unfiltered
  CTests, strict OpenSpec validation, and `git diff --check`.
- These are source, unit, integration, and local macOS regression results. No clean
  Ubuntu, Windows, or macOS run contains all three fixes yet. Keep `3.5`, `3.10`,
  `4.3`, `4.4`, `7.2`, `14.2`, and `14.9` unchecked and keep Agent/Codex read-only.

## 2026-08-10 Clean-Runner Results And Linux Strict-Clippy Repair

- Commit `560cf14784e92c5fec44ec6aac812b615d18524b` produced three clean-runner
  results. macOS run `31348302508` completed successfully. Ubuntu Rust Quality run
  `31348302487` passed formatting and locked unit tests, then failed `Run locked
  Clippy` with exit 101. Windows validation run `31348302510` passed the clean
  Unicode checkout and complete Rust Runtime step, installed Qt/OpenSSL, and then
  failed `Verify Windows Qt agent runtime` with exit 1. Public annotations do not
  identify the Windows compiler or CTest failure, so no complete Windows desktop,
  named-pipe, bootstrap-authentication, ConPTY, installer, or package result is
  inferred.
- The Ubuntu command was reproduced in a read-only `rust:1.97.1-bookworm`
  container. It reported unused macOS/Windows-only imports in
  `background_process_observation.rs` and `background_scheduler_lease.rs`, a
  Linux non-test dead-code constructor in `session_environment.rs`, and an
  unnecessary same-type `statvfs` cast in `durable_blob.rs`. Test imports now use
  their exact macOS/Windows guard; `ToolVariable::new` is compiled only for tests or
  the two production terminal platforms; and Unix available-space arithmetic uses
  a saturating `u128` product followed by checked `u64` conversion. Runtime behavior
  and every permission/Approval/mutation/execution/dispatch authority remain
  unchanged.
- Post-fix evidence passes Rust formatting and strict workspace all-target Clippy
  on native Windows and Linux `rust:1.97.1-bookworm`. Linux focused tests pass for
  the affected modules (`2`, `8`, `3`, and `10` matching tests), and locked Release
  workspace builds pass on both platforms. The complete Linux container run passes
  `876/878`; its two failures are existing Git commit-transaction fixtures, while
  the clean Ubuntu run above passed the locked test step before this source-only
  lint repair. Native Windows passes `881/882`; the remaining existing ConPTY test
  waits for an exit after receiving only `ESC[6n`, while the clean Windows run above
  passed its complete Rust step. These local environment-specific failures are not
  promoted to clean-runner evidence.
- A new clean Ubuntu run is required to close the strict-Clippy regression, and the
  Windows Qt failure still requires an authenticated log or a reproducing Qt 6.8.3
  environment before repair. Keep `3.5`, `3.10`, `4.3`, `4.4`, `7.2`, `14.2`, and
  `14.9` unchecked and keep Agent/Codex read-only.

## 2026-08-10 Windows Qt Render Diagnostics And Software Path

- At commit `b7b671aa6e533410a80986212020c90fb2ade230`, macOS run
  `31382263963` completed successfully. Windows run `31382263998` passed the
  clean Unicode checkout, all seven separated Rust gates, Qt/OpenSSL installation,
  CMake configuration, and the MSVC build before CTest failed. Its first pass named
  `tool_manager_runtime_registry`, `agent_workbench_render`, and
  `monaco_editor_render`; the failed-set rerun passed the ToolManager test and
  reproduced `agent_workbench_render` in 10.55 seconds. Public annotations stopped
  at the Monaco start line, so they do not identify the exact remaining failure
  diagnostics or root causes.
- The CTest environment had pre-populated
  `QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu`, which prevented the Monaco fixture's
  Windows-only complete Chromium flags from being installed. Windows now keeps the
  full bounded test-only software configuration in CMake: software OpenGL, preferred
  software RHI, disabled test sandbox, disabled GPU/compositing, and WebEngine
  context diagnostics. Non-Windows test behavior retains its prior flag.
- Both render fixtures prefix only static reviewed failures with
  `AEGISY_TEST_FAILURE:`. The Windows workflow publishes one combined failed-test
  annotation and one combined allowlisted diagnostic annotation, each capped at
  2,000 characters. Runner roots are regex-escaped and redacted case-insensitively
  in both slash forms before publication. It no longer spends one GitHub annotation
  per CTest line. Allowed diagnostic content is limited to that reviewed marker,
  the GLES context failures, the Chromium fatal context class, or a fixed high-level
  CTest fallback. The policy gate rejects non-prefix matching, per-line annotations,
  larger bounds, unrestricted fallback, weakened root redaction, rerun-exit
  substitution, or removal of the CMake Windows software path.
- Local verification passes workflow YAML parsing, the direct and registered
  `windows_packaging_policy` gate, focused `agent_workbench_render` and
  `monaco_editor_render`, the complete desktop build for both targets, and all
  `32/32` unfiltered CTests. The complete CTest run includes the full Rust workspace
  through `agent_runtime_protocol`.
- This is local regression and CI-observability evidence. A fresh clean Windows run
  must show whether the software path closes Monaco and must expose the first safe
  Workbench assertion if that independent non-WebEngine target still fails. Keep
  `3.5`, `3.10`, `4.3`, `4.4`, `14.2`, `14.9`, installer, package, and release
  gates open. Agent/Codex remains read-only.

## 2026-08-11 Windows Qt D3D11 And Fixed Diagnostic Follow-Up

- The preceding software-rendering path is historical and superseded. A Windows
  offscreen platform, software OpenGL/Qt Quick backend, or Chromium GPU disable can
  hide the production-shaped WebEngine presentation path. The current Monaco CTest
  instead sets `QT_QPA_PLATFORM=windows`, `QT_QUICK_BACKEND=rhi`,
  `QSG_RHI_BACKEND=d3d11`, and `QSG_RHI_PREFER_SOFTWARE_RENDERER=1`, and rejects
  `--disable-gpu` plus `--disable-gpu-compositing`. The preference requests a
  software adapter; this local configuration is not evidence that a Windows runner
  selected WARP or that Chromium used a particular internal adapter.
- The Qt 6 component gate and optional Monaco activation condition both require
  QuickWidgets, and the Monaco executable links `Qt6::QuickWidgets`. A two-case
  configure fixture rejects missing WebEngineWidgets and missing QuickWidgets. Its
  Windows branch requires `QQuickWindow::graphicsApi()` to report D3D11, locates
  WebEngine's actual child `QQuickWidget`, waits for its `QQuickWindow` scene graph
  to initialize, and requires that window's renderer interface to report D3D11. The
  assertions compiled in predecessor Windows run `31426799633`, but its generic
  failure marker does not prove which assertion executed or passed.
- Renderer failures no longer attach a parent console or copy arbitrary assertion
  text into `AEGISY_TEST_FAILURE:`. ToolManager and both renderer fixtures share one
  sink and an exact 50-code enum/mapping/workflow closure. Eight ToolManager codes
  cover command, registry, shim, and npm-residue phases; Workbench and Monaco use
  stage-specific codes rather than `AWB_ASSERTION`/`MONACO_ASSERTION`. Injected tests
  cover partial native writes, initial native failure with checked CRT fallback,
  failure after any native byte without channel splitting, fixed-code mapping, and
  bounded printable local detail. Two CTests execute each real GUI binary's early
  self-test and require exit `86`, empty stdout, and exactly one fixed stderr line.
- Windows run `31426799633` at
  `4d3e10cde48531d17646058814e32ab49b00d986` reached the Qt CTest gate and failed
  tests 8 (`tool_manager_runtime_registry`), 18 (`agent_workbench_render`), and 33
  (`monaco_editor_render`). Its diagnostic annotation contained only the predecessor
  `AWB_ASSERTION` and `MONACO_ASSERTION`. This is native execution evidence for the
  predecessor binaries, not evidence of a failing stage, D3D11/WARP/Chromium success,
  or a repaired Windows test. Every other registered CTest passed, including Runtime
  environment, named-pipe/bootstrap, generated AAP, and ConPTY coverage. Those are
  predecessor component results only; they do not prove the current 50-code repair,
  the complete current workflow, an installer, a package, signing, or release.
- ToolManager's residue fixture is now unskippable and independent of host-installed
  CLIs. A test-only command path points at Unicode fake `node`/`npm` shims. Both
  require exactly five ordered arguments,
  `list -g opencode-ai --depth=0 --json`; Windows compares those tokens without
  regard to case, while POSIX compares them exactly. Windows uses `_wputenv_s` to
  match Qt's CRT reads, normalizes inherited empty to unset, and an executable nested
  guard proves Unicode original/override/readback plus inherited state restoration.
- The Windows Qt step explicitly keeps native-command error handling manual, discards
  the initial CTest body without retaining it in memory, and stores its exit code once.
  The failed-set rerun is processed as a stream. A `List<string>` retains at most 50
  fixed marker/mapped graphics codes, while a `Queue<string>` retains at most 20
  allowlisted high-level CTest lines for fallback. Before fallback, the exact Qt
  6.8.3 DXGI-factory, D3D11 device/context, and `ID3D11DeviceContext1` failure
  prefixes map to `QT_D3D11_INITIALIZATION`; no HRESULT or other dynamic suffix is
  retained. No complete rerun array exists.
  Public output is limited to the three reviewed `Write-Output` calls: one fixed
  no-name failure or bounded failed-name annotation sourced only from
  `LastTestsFailed.log`, and one bounded diagnostic annotation. Encoding precedes
  the 2,000-character cap, and only the diagnostic annotation receives runner-root
  redaction assigned back before publication.
- `LastTestsFailed.log` contributes at most 50 entries matching one strict ASCII
  `index:test-name` grammar. Rerun lines longer than 4,096 characters are ignored;
  fallback stores only six fixed CTest categories, never raw `$line`. The Qt step is
  unique and content-SHA-bound. The normalized `Testing/Temporary` path, both CTest
  log names, annotation titles, fixed-code prefix, and private Qt diagnostic state
  must remain inside it. Output-command and `Write-Output` count checks are case-
  insensitive.
- The policy gate independently derives the `FailureCode` enum set, case-label set,
  returned fixed strings, and workflow allowlist from one canonical 50-code list and
  requires a one-to-one mapping. The ToolManager test macro must occur exactly once
  in the exact `AegisyToolManagerRuntimeTest PRIVATE` compile-definition command.
  Its negative matrix rejects extra enum aliases, swapped mappings, permissive marker
  regexes, raw marker lines, secret/dynamic graphics suffixes using either `$line` or
  `$_`, permissive D3D11 prefix matching, a missing QuickWidgets activation/link
  gate, unbounded initial capture, public streamed output, lost queue/list bounds,
  discarded redaction, guard reordering, extra exit assignments, renderer setting
  duplication/reordering, offscreen/software paths, GPU-disable flags, duplicate
  publishing steps, external raw `LastTest.log`/wildcard-log access, borrowed Qt
  diagnostic state, one-sided marker-set drift, test-macro target/scope drift,
  case-varied console output, early or comment-spoofed stages, alternate Helper stage
  construction, comment-spoofed D3D11 bindings, permissive npm arguments, and
  Win32-only environment writes.
- Verification executed from `/Users/cwm/aegisy-app`: CMake reconfigure and complete
  build passed; after the final stage/policy/ToolManager repairs, the focused suite
  passed `6/6`, ToolManager passed twenty consecutive registry runs, and the final
  post-hardening serial unfiltered desktop suite passed `34/34` in 186.95 seconds,
  including `agent_runtime_protocol` in 134.17 seconds, both real failure-channel
  probes, and the two-case Qt6 release policy. Rust formatting, locked workspace/all-target
  strict Clippy, and locked Release build passed earlier in the same slice with no
  Rust source change afterward. The direct and registered Windows policy checks,
  Ruby/Psych YAML parse, strict OpenSpec validation, and `git diff --check` passed.
- This is local macOS implementation and regression evidence. No current commit has
  compiled the current 50-code/stage-specific repair with MSVC or executed it under
  the Unicode Windows checkout. The predecessor run above does not prove WARP,
  Chromium renderer, Workbench failure repair, TLS, installer, package, signing, or
  release. It does provide the explicitly bounded predecessor component evidence for
  named-pipe/bootstrap/ConPTY stated above, which is not a substitute for a green
  current workflow. Keep `3.5`, `3.10`, `4.3`, `4.4`, `14.2`, and `14.9` unchecked
  and keep
  Agent/Codex read-only.

## 2026-08-11 Windows ConPTY Post-Interrupt Readiness Follow-Up

- At commit `683449bb5ee772990f4e1d20f70e636236d9c573`, macOS run
  `31449651947` completed successfully through the full build and exact unfiltered
  CTest gate. Windows run `31449651952` passed the Unicode checkout, Rust toolchain,
  and formatting, then failed `Test Windows agent runtime` at
  `terminal::tests::conpty_interrupt_keeps_shell_alive_and_preserves_ansi` with
  `897` passed, one failed, and `122.58s` total test time. All later Rust, Qt,
  CTest, installer, package, and publication gates were skipped.
- The bounded public annotation includes neither a panic location nor assertion
  detail. Because the fixture is wrapped by a 120-second stage timeout and the
  reported target time is `122.58s`, a stage timeout is a strong inference, not a
  proven assertion diagnosis. No `agent-runtime` source changed between predecessor
  commit `4d3e10c` (where the ConPTY component passed) and `683449b`; that comparison
  supports a scheduling/fixture diagnosis but does not replace a repaired Windows
  execution.
- The interrupt fixture previously slept a fixed 250 ms after sending Ctrl+C and
  then immediately sent the ANSI marker plus `exit 23`. It now sends a shell-specific
  readiness command first. cmd receives `echo AEGISY_INTERRUPT_^COMPLETE`, while
  PowerShell receives `Write-Output ('AEGISY_INTERRUPT_' + 'COMPLETE')`. In both
  cases the typed/echoed command omits the complete marker; only successful shell
  execution emits `AEGISY_INTERRUPT_COMPLETE`. The fixture waits for that exact
  output before sending the ANSI and exit commands. Production terminal code,
  timeouts, output interpretation, and authority are unchanged.
- Local verification in `rust:1.97.1-bookworm` passes Rust formatting, strict
  workspace/all-target Clippy, the locked Release workspace build, and the two
  platform-neutral ANSI helper tests. The first complete workspace run reproduced
  the two documented base Git transaction fixture failures and one transient Git
  version fixture failure; the Git version fixture passed its exact rerun. With only
  the two documented Git transaction fixtures skipped, every remaining workspace
  library, binary, integration, and doc-test target passes. The Windows-only repaired
  fixture cannot execute on this host.
- No OpenSpec checkbox closes from this repair. A fresh complete clean Windows run
  must pass the Rust workspace and then reach the generated Qt/C++ consumer,
  reconnect/runtime, named-pipe/bootstrap, renderer, installer, and package gates.
  Keep `3.5`, `3.10`, `4.3`, `4.4`, `14.2`, `14.9`, `23.10`, and all Windows
  release gates unchecked. Agent/Codex remains read-only.

## 2026-08-12 Non-Turn Outcome Time-Ordering Integrity

- Schema-v23 terminal-outcome admission and persisted graph validation now enforce
  the exact invariant
  `reserved_at_ms <= observed_at_ms <= recorded_at_ms`. The earlier admission
  check proved only that recording followed reservation and observation; it did not
  reject an outcome observed before its reservation. The replacement transitive
  check rejects that case before the outcome event, row, reservation CAS, or Session
  sequence can change. Direct reads and startup validation independently reject the
  same hash-consistent persisted drift and `open_or_recover` enters
  `ReadOnlyRecovery` with `workbench-database-integrity-failed`.
- Two focused fixtures cover the missing boundary. One reserves at `20`, observes at
  `19`, records at `20`, requires
  `mutation-reservation-outcome-time-invalid`, and compares the complete graph before
  and after to prove zero writes. The other first creates a valid terminal graph,
  changes only `reserved_at_ms` to move it after the persisted observation while the
  immutable Trigger is temporarily removed, restores the schema Trigger, then
  requires direct-read rejection and read-only recovery after restart.
- The focused `non_turn_mutation` suite passes `50/50` in
  `rust:1.97.1-bookworm`. Rust formatting, the repository CI-equivalent locked
  workspace/all-target Clippy command with `-D warnings`, and the locked Release
  workspace build pass. Strict OpenSpec validation and `git diff --check` pass.
  The complete `aegisy-agentd --lib` run passes `894/896`; its only failures are the
  two documented base Git transaction fixtures
  `previews_and_commits_only_agent_delta_while_preserving_user_index_and_worktree`
  and
  `injected_ref_failure_rolls_back_and_external_ref_rewrite_is_preserved`. With only
  those exact fixtures excluded, the remaining library tests pass `894/894`.
- At base commit `73b841f2beaedcc7cd911ec7664c8027cb26548a`, macOS run
  `31451782643` succeeded. Ubuntu run `31451782669` failed `Run locked unit tests`,
  and Windows run `31451782650` failed `Test Windows agent runtime`; the Windows
  failure remains
  `terminal::tests::conpty_interrupt_keeps_shell_alive_and_preserves_ansi`.
  These failures require separate diagnosis and do not prove this Store repair on
  those runners.
- No OpenSpec checkbox closes. This adds no AAP method, Qt surface, production
  producer, consumption route, dispatch, filesystem/Git/job mutation, genuine user
  Approval, or authority. Keep `3.6` unchecked and Agent/Codex read-only.

## 2026-08-13 Non-Turn Reservation Consumption Ledger

- Schema v24 adds the crate-internal `mutation_reservation_consumptions` ledger and
  strict `mutation-reservation-consumption-receipt/0.1` contract. Source-first
  `c0 -> c1 -> c2` consumption is independent of core reservation `r1/r2`; valid
  histories are exactly empty, source-only, source-plus-terminal, or
  source-plus-reconciliation. Receipts bind the exact existing internal event,
  evidence identity, prior source receipt, revision pair, and time, with every
  authority fixed false and no success/result claim.
- Exact retry executes before write admission and preserves the first receipt/time
  with zero writes. New source insert or resolution CAS reclassifies under one
  `IMMEDIATE` lock and rechecks core/consumption revision, owner, archive/pending
  deletion, project/root/Turn scope, evidence, anchor, and time. Tests cover all four
  source kinds, terminal and reconciliation paths, resolution-before-source, legacy
  denial, invalid revisions, sampled low space, exact peer replay, peer core drift,
  archive/deletion races, and insert/update/final-commit rollback. A dedicated
  source-INSERT race proves sampled low space cannot hide an exact peer commit: the
  caller receives the peer's first receipt/time, only the ledger count changes, both
  internal/Public sequences remain fixed, and evidence/core-revision drift conflicts.
- Startup preserves `c0/c1` while moving open core `r1` reservations to
  reconciliation-required `r2`. The whole-Store verifier now performs a bounded
  semantic scan of every consumption identity and rebuilds both receipts. Field,
  phase, evidence, anchor, owner/kind, authority, and time drift enter read-only
  recovery. A final `r2` graph requires a source receipt claiming `r2` to be no
  earlier than the core transition and a source receipt claiming `r1` to be no later
  than that transition. One regression proves a future-dated `r1` receipt rolls back
  outcome admission. Two further regressions recompute both receipt identities and
  prove the otherwise hash-consistent early-`r2` and late-`r1` time forgeries both
  fail direct whole-Store verification and restart. Contract tests independently
  cover terminal and reconciliation previous-receipt drift, equal/reversed event
  sequence, reversed event time, and reversed consumption time with recomputed
  receipt identities.
- The v23-to-v24 migration validates and backs up the exact v23 graph, adds an empty
  ledger/index/Triggers, preserves reservation/source/outcome/events and both
  internal/Public sequence states, and fabricates no receipt. Table, index, and
  Trigger name collisions each roll back with `user_version = 23`. Session purge
  deletes a real c2 row before outcome/source/reservation dependencies and leaves no
  consumption row.
- Verification used `rust:1.97.1-bookworm` in container `aegisy-v24-rust`:
  `cargo fmt --all -- --check`, `cargo check -p aegisy-agentd --lib --locked`,
  `cargo test -p aegisy-agentd --lib mutation_reservation_consumption --locked`
  (`9/9`), `cargo test -p aegisy-agentd --lib non_turn_mutation_consumption --locked`
  (`13/13`), `cargo test -p aegisy-agentd --lib non_turn_mutation --locked`
  (`63/63`), `cargo clippy --workspace --all-targets --locked -- -D warnings`, and
  `cargo build --workspace --release --locked` pass.
- The complete `cargo test -p aegisy-agentd --lib --locked` run reports `919/921`;
  its only failures are the two documented base Git transaction fixtures
  `previews_and_commits_only_agent_delta_while_preserving_user_index_and_worktree`
  and `injected_ref_failure_rolls_back_and_external_ref_rewrite_is_preserved`.
  Excluding exactly those two fixtures passes `919/919`. One intermediate excluded
  run saw the unrelated environment-sensitive
  `in_memory_project_list_preserves_the_opened_root_identity` fail after temporary
  root deletion; its exact isolated rerun passed, and the final `919/919` run also
  passed it.
- `openspec validate build-aegisy-agent-workbench --strict`, `git diff --check`, and
  the checkbox-diff gate pass. OpenSpec `3.6`, `5.1`, and `5.2` remain unchecked.
  This slice adds no production producer, external caller-CAS or AAP/Qt consume
  route, Public Timeline event, dispatch, filesystem/Git/job mutation, genuine user
  Approval, or authority. Agent/Codex remains read-only.

## 2026-08-23 Schema-v24 Startup Clock Regression

- Startup reconciliation now derives each `r1 -> r2` transition time from the
  maximum of the current observation, reservation time, and any validated `c1`
  source receipt consumption time. This preserves the existing v24 ordering rule
  when the wall clock moves backward after source consumption.
- A deterministic fixture uses timestamps immediately below the JSON-safe integer
  ceiling, reopens under the necessarily earlier host clock, verifies the Store
  remains writable with the exact `c1` receipt and `r2` transition time, then
  consumes the bound reconciliation receipt at `c2`.
- This is a recovery-integrity repair only. It adds no AAP/Qt route, production
  producer, external caller CAS, dispatch, Approval, mutation, or execution
  authority; OpenSpec `3.6` remains unchecked and Agent/Codex remains read-only.

## 2026-08-23 Aegisy Companion Product Reset

- `proposal.md`, `roadmap.md`, `design.md`, and section 0 of `tasks.md` now make
  website-backed configuration, repair/rollback, extensions, Chinese UX, Skills,
  MCP, diagnostics, gateway, updater, and desktop enhancement the active roadmap.
  Existing Workbench tasks remain retained long-horizon reference.
- The main Qt navigation is configuration-first and labels the only integrated
  programming destination `Codex 编程`. The retained WebEngine preview identifies
  Codex and no longer displays a Claude model.
- `product_scope_policy` verifies all four CLI configuration targets remain
  supported while Runtime compiles only `codex_adapter` and contains no
  Claude/Gemini/ACP adapter module. It also binds the visible navigation to the new
  proposal and companion capability spec.
- The complete `AegisyClient` target and `AegisyProductScopePolicyTest` build. The
  focused CTest set passes `5/5`: product scope, tool runtime registry, gateway
  configuration, desktop enhancement/history, and Skills installation/routing.
  Strict OpenSpec validation and `git diff --check` pass.
- This is a product-scope reduction. It does not remove existing safety tests and
  grants no Agent-authored write, command, Git, Approval, remote, background, or
  multi-agent authority.

## 2026-08-23 Companion Website Observation Foundation

- `CompanionConfigProjection` validates a bounded exact-field
  `aegisy-companion-config-projection/0.1` snapshot, hashes website account/Key
  identities, strips credentials and raw IDs, rejects duplicate/secret-shaped
  metadata, infers no model data, fixes configuration authority/applied false, and
  isolates the last-valid display cache by account identity.
- API account/Key requests bind auth generation and exact request/source URL, reject
  redirects, cache use, wrong Content-Type, oversized pages, incomplete pagination,
  stale account responses, and untrusted origins. Failed projection publishes no raw
  Key signal; successful explicit consumers receive raw Keys synchronously before
  the accumulator is cleared.
- Profile schema 7 derives every Profile SecureStorage ref from a strict local UUID. A
  tampered QSettings ref cannot read, overwrite, or delete another SecureStorage
  item. Persisted credential hints now use a domain-separated SHA-256 fingerprint,
  never a Key substring. Hashed website account/Key/projection bindings persist with
  Profiles, while invalid or partial bindings fail closed.
- `CompanionCredentialBroker` stages a complete validated batch into derived
  SecureStorage slots with rollback, emits only opaque handles, and rejects
  cross-account/cross-Key resolution. ConnectWizard consumes the sanitized
  projection and never stores a website Key in combo item data; product scope policy
  locks that source boundary.
- `CompanionModelProjection` accepts website-Key and local-Profile bindings, rejects
  duplicates, secret-shaped IDs, surrounding-whitespace normalization and authority
  drift, and omits every provider-body field. ApiClient model transport binds and
  retires unique pending requests and checks exact URL, redirect, Content-Type,
  identity encoding, Content-Length/final bytes, auth epoch, account, current source
  projection, Key, handle, and platform. ConnectWizard validates the result contract
  and current tuple and contains no global `modelsReceived` subscription.
- ModelsDialog now presents only sanitized active candidates and uses the same exact
  request-specific model projection. It has no editable/manual Key input, Key
  fragment, raw-Key subscription, or global model subscription; product scope policy
  locks these source constraints.
- The complete desktop target builds and the focused set passes `7/7`: companion
  configuration projection, credential broker, model projection/correlation,
  account/API fail-closed behavior, Profile activation/source binding, ToolManager
  gateway config/fingerprint parity, and
  product scope policy. This is not `0.2` completion: model integration into the revisioned configuration cache, cache
  authenticity/revision/expiry, and encrypted credential-bearing config backups
  remain open.

## 2026-08-23 Chat Companion Credential Migration

- Chat now consumes only validated active companion candidates and the correlated
  model projection. Combo item data contains the opaque credential handle plus
  account, hashed Key, source projection, platform, and safe display/group metadata;
  it contains no credential plaintext or Key fragment. Chat no longer subscribes to
  raw Key/global model signals or retains the raw Key inventory.
- ApiClient companion entry points cover streaming chat, Chat image Skill, and Chat
  presentation Skill requests. Each entry revalidates the current auth epoch,
  verified account, current source projection, Key, handle, platform, and reviewed
  website origin before resolving the credential once through
  `CompanionCredentialBroker`. Auth/origin/projection replacement retires bound
  operations. Presentation retries and completions recheck their exact binding;
  retired late results are inert.
- Chat history schema 2 persists only a valid hashed website-Key identity and bounded
  safe display name. Legacy raw `key_id` values are ignored and never rewritten.
  Active Profile matching uses the hashed website account/Key binding without
  loading Profile credential plaintext.
- The complete desktop target builds. Focused CTest passes `8/8`: Profile activation,
  ToolManager runtime and gateway configuration, product scope policy, companion
  configuration projection, credential broker, model projection, and account/API
  fail-closed behavior. Strict OpenSpec validation and `git diff --check` pass.
- OpenSpec `0.2` remains unchecked: revisioned
  authenticated model/cache state, and encrypted credential-bearing
  backups remain open. This grants no Agent/Codex write, command, Git, Approval,
  remote, background, or multi-agent authority.

## 2026-08-23 Standalone Image Companion Migration

- The standalone image tool consumes only active SecureStorage-backed `gpt-image`
  candidates from the validated companion projection. Its widgets contain only the
  opaque handle plus account, hashed Key, source projection, platform, group, and
  safe display metadata; the raw Key inventory and credential fragments are absent.
- Generation uses one unique `image-dialog-*` request ID and
  `generateCompanionImage`. Only the matching `companionImageGenerated` or
  `companionImageFailed` signal may change the preview/status. The existing ApiClient
  auth/origin/projection retirement boundary makes late results inert.
- The complete desktop target builds. The focused companion/API/Profile/Tool/product-
  scope set passes `8/8`; strict OpenSpec validation and `git diff --check` pass.
- OpenSpec `0.2` remains unchecked because revisioned
  authenticated model/cache state, and encrypted credential-bearing backups remain
  open. Agent/Codex remains read-only.

## 2026-08-23 Companion Usage Projection Migration

- `aegisy-companion-usage-projection/0.1` binds safe per-Key usage rows to the
  hashed website account and exact current configuration projection. Rows contain
  only hashed Key identity, bounded display/group/state, and non-negative cost/quota
  metrics. Raw website Key IDs and credential values are absent, while raw-ID,
  credential, and configuration-authority flags are fixed false.
- ApiClient retains only a minimal account/auth/projection-bound in-memory mapping
  from hashed Key identity to raw website ID plus bounded quota values. The raw IDs
  enter only the authenticated website request body. Response handling binds exact
  request ID, auth generation, current account/projection/origin/final URL, redirect,
  JSON Content-Type, identity encoding, and 1 MiB bounds; unexpected raw response IDs
  fail closed before the validated projection is emitted.
- UsageDialog consumes companion configuration and the exact correlated usage
  projection. It no longer subscribes to raw Keys, calls the raw-ID method, or stores
  a raw-ID-keyed usage object. Product scope policy locks those source constraints.
- The complete desktop target builds, and the focused companion/API/Profile/Tool/
  product-scope set passes `9/9`. Strict OpenSpec validation and
  `git diff --check` pass.
- OpenSpec `0.2` remains unchecked because model results are not merged into a
  revisioned authenticated cache and credential-bearing backups remain unencrypted.
  Agent/Codex is read-only.

## 2026-08-23 Companion API-Key Management Migration

- `aegisy-companion-key-management-projection/0.1` is a strict online-only contract
  bound to the verified website account and exact current configuration SHA. It
  exposes only safe Key/group metadata plus globally unique 256-bit system-random
  group/create/test/update/delete handles. Raw Key/group IDs, credentials, credential
  fragments, configuration authority, and mutation authority are absent/false.
- ApiClient owns the only live mapping from those action-scoped handles to raw IDs
  and SecureStorage credential handles. Handles expire after 15 minutes and every
  operation rechecks auth generation, account, configuration SHA, management SHA,
  action, Key/group identity, origin/final URL, redirect, cache, encoding, JSON type,
  and 256 KiB bounds. Raw path segments are percent-encoded. Provider bodies and
  create response data never enter result signals. A dispatched mutation retires the
  management context and requires full refresh; stale responses are inert. Delete
  reports local credential cleanup separately.
- ApiKeysDialog consumes only the management projection and exact correlated
  operation/model results. Copy/reveal, raw preferred ID persistence, Key fragments,
  raw inventory/group signals, raw CRUD/test calls, and global operation/error signals
  were removed. MainWindow's redundant raw signal was removed, startup deletes both
  legacy plaintext/preferred-ID settings, and ApiClient no longer exposes or emits
  the legacy raw list/group/CRUD/test APIs.
- The new projection test covers exact fields, raw/credential absence, handle shape
  and cross-action uniqueness, group binding, metric/date/state/display bounds,
  unknown fields, authority drift, uppercase secret shapes, and fractional counts.
  ApiClient tests prove unverified read/create/update/delete/test fail before network;
  product scope statically rejects every legacy public/UI/MainWindow boundary.
- The complete desktop target builds and the focused companion/API/Profile/Tool/
  product-scope set passes `11/11`. The trusted-origin fake transport covers two-read
  handle rotation, stale-handle rejection before network, encoded raw path segments,
  positive update/create, strict response-code types, create credential staging,
  concurrent-mutation rejection, and auth-change `outcome-unknown` with a late reply
  inert. Contract, preflight, credential-rebind, and static raw-boundary regressions
  also pass. A dedicated Qt dialog race/render fixture, held Key-test rotation, and
  injected cleanup failure remain open. Strict OpenSpec validation and
  `git diff --check` pass. `0.2` stays unchecked for that evidence, authenticated
  revisioned cache/model integration, and encrypted credential-bearing backups.
  Agent/Codex is read-only.
