# Aegisy Workbench Feature And Channel Policy

Status: approved policy for OpenSpec `1.8`

Last reviewed: 2026-07-26

## Purpose

This policy defines how a Workbench feature moves through `internal`, `preview`,
`beta`, and `stable`, how a future `remote` surface is kept separate, and how a
signed `emergency-disable` can revoke capability. It does not turn documentation
into runtime authority: a feature is usable only when every code, protocol,
security, persistence, platform, and release gate also passes.

## Independent Axes

The policy has three independent axes. They must not be represented by one boolean.

| Axis | Values | Meaning |
| --- | --- | --- |
| Maturity channel | `internal`, `preview`, `beta`, `stable` | Who may discover and opt into a feature, and what evidence is required. |
| Execution surface | `local-desktop`, future `remote` | Where UI, Runtime, workspace, and credentials live. Remote is not a more mature local channel. |
| Revocation | `normal`, `emergency-disabled` | A signed, monotonic override that can only remove authority. |

Each feature also has a risk class: `ui-only`, `read-only`, `local-mutation`,
`process-or-network`, or `background-or-remote`. Higher classes inherit every lower
class gate and add their own permission, approval, sandbox, recovery, audit, and
release requirements.

The signed package channel is an upper bound, not permission. An `internal` package
may contain features declared at any maturity for controlled testing; a `preview`
package may contain preview, beta, or stable features; a `beta` package may contain
beta or stable features; and a `stable` package may expose only stable features.
Neither a server response nor QSettings can raise a feature above the maturity and
code capabilities declared by the signed artifact.

## Channel Contract

| Channel | Discovery and eligibility | Data and compatibility | Authority and release gates |
| --- | --- | --- | --- |
| `internal` | Developer/test builds or explicitly managed staff cohort; hidden from ordinary users | Test data or an explicitly isolated profile; schemas may change only with the repository migration and rollback rules | No production claim. Mutation remains disabled unless the exact feature separately passes all security gates. |
| `preview` | Signed build, explicit user opt-in, visible limitations and exit path | Durable data must be forward readable, exportable, or rollback-safe; disabling the UI does not delete its database | Read-only is the default. A preview label cannot bypass capability negotiation, permission, approval, sandbox, recovery, or supported-platform gates. |
| `beta` | Signed, eligible cohort plus explicit opt-in unless Product records a narrower managed rollout | Migration backup, downgrade/rollback behavior, retention, support diagnostics, and incident runbook are required | Local mutation may be enabled only for the exact reviewed feature and platform. No unattended/background or remote authority is implied. |
| `stable` | Default-visible only on release-supported platforms | Compatibility, retention, export/delete, migration, update, and rollback are release contracts | Complete security, privacy, accessibility, performance, clean-machine, support, and release-owner evidence is mandatory. Unknown policy or capability state fails closed. |
| `remote` | Unavailable in every channel until a separate OpenSpec and security review approves an exact architecture | Must define transport authentication, workspace custody, encryption, tenancy, retention, regional/data controls, reconnect, revocation, and incident recovery | No local flag, URL, hidden environment variable, or emergency allow can enable it. Remote messaging/background control also requires the autonomy gates. |
| `emergency-disabled` | Applied to any eligible channel by a valid signed policy | Existing verified local history remains readable where safe; the override does not erase data or run migrations | Can only subtract new-work authority. It cannot enable, promote, approve, retry, dispatch, or widen a permission/capability. |

## Evaluation Order

The effective decision is the intersection of all applicable authorities, in this
order:

1. **Build inclusion:** absent code/assets cannot be enabled by configuration.
2. **Emergency revocation:** a valid disable wins over every ordinary setting.
3. **Managed policy:** enterprise restrictions may narrow channel, feature, model,
   workspace, network, retention, and extension availability.
4. **Release eligibility:** artifact channel, supported platform, cohort, account,
   and explicit opt-in must match the feature registry.
5. **AAP capability negotiation:** both host and Runtime must advertise and enforce
   the exact stable capability; experimental declarations are never assumed.
6. **Context gates:** project trust, Session mode/state, Runtime health, Store
   integrity, reconciliation, model compatibility, permission, approval, sandbox,
   budget, and recovery must all allow the requested operation.

Any missing, malformed, stale, expired, contradictory, unsupported, or unverified
input produces `unavailable` or `blocked`, never an implicit allow. A lower layer
cannot restore authority removed by an earlier layer.

## Feature Registry Contract

Before a feature is reachable outside tests it must have one repository-owned entry
containing:

- stable feature ID and accountable Product, Engineering, Security, and Release
  owners;
- risk class, minimum channel, permitted execution surfaces, and supported platform
  rows;
- compile/build inclusion and exact AAP capabilities or host-only boundary;
- default state, cohort/opt-in behavior, managed-policy key, dependencies, conflicts,
  and emergency-disable scope;
- persistent data/schema versions, disable behavior, rollback/forward-compatibility,
  retention, export/delete, and recovery behavior;
- telemetry/diagnostic categories with content excluded by default;
- positive, denial, rollback, stale-policy, offline, migration, accessibility,
  performance, and cross-platform evidence; and
- promotion date/gate, known limitations, last reviewed version, and rollback owner.

Unknown feature IDs are ignored for display and denied for authority. Registry
entries cannot carry secrets, arbitrary executable names, host paths, prompts,
provider bodies, or unsigned endpoint overrides.

## Promotion Rules

A promotion is a reviewed repository and release change, not a server-side label
flip. For the exact feature and platform:

1. all dependency OpenSpec tasks and risk-class gates are complete;
2. protocol/schema compatibility and capability-denial fixtures pass;
3. migration, downgrade, disable, recovery, and rollback are proven without data
   loss or fabricated success;
4. privacy fields, diagnostics, retention, and support runbooks are reviewed;
5. accessibility, IME, scaling, performance, installer, update, and clean-machine
   evidence passes the supported-platform matrix;
6. Product, Security, and Release owners sign the promotion and rollback record; and
7. the prior channel remains available as a rollback target until the observation
   window closes.

Promotion is per feature. A stable shell does not make every Runtime capability
stable. A feature may move backward immediately when evidence regresses; data must
remain readable or enter an explicit recovery state.

## Persistence And Rollback

- Hiding or disabling a feature never drops tables, rewrites history, purges Blobs,
  removes checkpoints, or marks uncertain work successful.
- Schema migration follows the Workbench backup/integrity boundary. A flag cannot
  choose an incompatible database interpretation.
- Ordinary opt-in and UI preferences may use bounded non-secret local settings.
  Security authority, approval, permission, model tokens, and signing material may
  not be stored as ordinary flag values.
- A rollback may use the last artifact whose host, sidecar, adapter, schema, and
  updater compatibility are verified. Downgrading across an unsupported schema
  enters read-only recovery instead of guessing.
- Disabling new work preserves cancellation, terminal stop, shutdown, diagnostic
  export, and other reviewed cleanup/read paths where their independent capabilities
  remain valid.

## Remote Policy

`remote` is currently fixed unavailable. Enabling it requires a separate accepted
OpenSpec that defines at minimum:

- authenticated and peer-verified transport, device/session binding, revocation,
  replay resistance, rate and frame bounds;
- workspace custody and filesystem identity, encryption at rest/in transit, tenant
  isolation, region, retention, deletion, export, backup, and incident semantics;
- scoped token issuance with no desktop login or long-lived provider credential in
  web content, URLs, process arguments, logs, or Agent protocol payloads;
- permission, genuine-user Approval, sandbox, network policy, checkpoint, mutation
  acknowledgement, cancellation, reconnect, and reconciliation across disconnects;
- browser origin/CSP/CSRF/XSS controls and a clean separation from local WebEngine;
  and
- background/multi-agent gates, budgets, notifications, support ownership, and a
  remote emergency stop.

Local preview/beta success cannot satisfy these requirements.

## Emergency Disable Contract

The current foundation uses authenticated HTTPS after login to fetch a bounded
signed `aegisy-workbench-emergency-policy/0.1` envelope. The inner policy is verified
with the pinned Ed25519 updater public key and contains only schema, positive
sequence, issue/expiry time, disable state, bounded reason code, and signature.

The emergency contract is:

- only a valid higher sequence changes policy; same-identity replay may repair an
  interrupted cache write, while rollback or conflicting content fails closed;
- redirects, non-HTTPS origins, malformed/oversized bodies, unknown fields, expired
  policy, invalid signatures, and ambiguous cached high-water state cannot grant
  authority;
- cache and high-water metadata contain no prompt, path, provider body, credential,
  user message, or arbitrary server text;
- disable is enforced at both the Qt request boundary and Runtime method boundary;
- emergency Runtime startup never launches Codex and exposes only reviewed local
  read/recovery capabilities; new Session, Turn, workspace mutation, portable import,
  and Runtime restart are denied with the stable emergency error;
- transition retires the normal sidecar generation with bounded cleanup and never
  infers an interrupted Turn or process successful; and
- an emergency policy can never enable a feature, select a model, grant permission,
  record Approval, dispatch work, retry a mutation, or change release channel.

Version `0.1` is a global Workbench disable. Feature-scoped revocation requires a
new reviewed schema/version and cannot be added as an unknown field or interpreted
from the bounded reason code. The current Qt and Rust emergency route/capability
allowlists are independently maintained; every new method must default-deny and a
parity test or future machine-readable classification must prove they remain equal.

The production publisher/route, OS-secure anti-deletion anchor, healthy-Store
diagnostic bundle, and signed macOS/Windows end-to-end evidence remain release gaps.
First-install absence is not a verified allow policy and must be resolved before an
emergency system is described as a complete production control plane.
The current Runtime receives a derived
`AEGISY_WORKBENCH_EMERGENCY_DISABLED=1` process flag rather than independently
verifying the exact signed policy identity; authenticated bootstrap binding of that
identity is also a production gap. The environment flag is defense in depth for the
partial foundation, not a final policy authority.

## Diagnostics And Audit

Feature decisions may record only bounded content-free fields: feature ID, registry
version, requested/effective channel, surface, result/reason code, policy source and
sequence identity, negotiated capability identity, platform row, and timestamp.
They must not record account identifiers, cohort source values, prompts, source,
paths, terminal output, diffs, provider bodies, credentials, or raw policy bodies.

Support UI must distinguish `not included`, `not eligible`, `opt-in required`,
`capability unavailable`, `policy blocked`, `emergency disabled`, `recovery only`,
and `unsupported platform`. It must not reduce these states to a misleading generic
toggle.

## Current Baseline

- The Workbench is an engineering/internal preview; this policy does not promote it.
- Agent/Codex remains read-only. User-origin editor saves are a separate constrained
  operation and do not establish Agent mutation authority.
- Remote Workbench, remote messaging control, background dispatch, child-agent
  execution, unattended writes, and genuine-user Approval remain unavailable.
- The signed emergency-disable foundation exists with the gaps listed above.
- A production feature registry and general cohort delivery service are not yet
  implemented. Until they are, channel selection cannot be represented as runtime
  execution authority.

## Required Verification

- Table-driven coverage crosses package channel, feature maturity, opt-in, managed
  and server denial, platform/capability gates, and emergency state.
- Property tests prove that adding a restriction never adds a capability, emergency
  output is always a subset of normal output, promotion cannot create missing
  permission/approval/sandbox/model authority, and unknown input fails closed.
- Signed-policy tests cover exact fields, signature/key/time/lifetime, sequence
  rollback, same-sequence conflict, higher-sequence recovery, partial cache writes,
  missing halves, full cache deletion, clock rollback, offline/stale behavior, and
  the first-install compatibility exception.
- Qt/Runtime parity covers immediate pre-reconnect denial, route/capability
  allowlists, `normal -> emergency -> normal`, no-Codex emergency startup, corrupt
  Store recovery, local history/export preservation, and inert old-generation data.
- Race and crash tests switch policy around queued, new, and active operations,
  heartbeat Unknown, Timeline subscription, sidecar/app crash, and upgrade/rollback
  without duplicate dispatch or fabricated terminal success.
- Signed macOS and Windows package tests prove channel eligibility, absence of
  production debug authority, compatible rollback, legacy login/gateway/update
  availability, and content-free diagnostics.
- Until remote is approved, negative tests prove there is no remote UI, AAP route,
  capability, scheduler, notification dispatch, or hidden endpoint override.

## Ownership And Change Control

Product owns channel intent and user disclosure. Product Security owns authority
intersection, emergency policy, remote gates, and hostile-state behavior. Runtime
and Desktop Platform own double enforcement and capability truth. Data Governance
owns retention/export effects. Release Engineering owns artifact-channel identity,
promotion evidence, rollback, and incident execution.

Any change to precedence, emergency behavior, remote availability, persistent flag
state, or mutation eligibility requires OpenSpec review and a matching
`PROJECT-MEMORY.md` update.
