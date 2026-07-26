# ADR 0009: Public Product Name

- Status: Proposed
- Accountable owner: Product
- Consulted owners: Brand, Legal, Documentation, Support
- Due gates: OpenSpec `1.2`; before public beta and end-user concepts under `23.1`

## Context

The repository currently uses `Aegisy Coding` for the product goal, `Agent
Workbench` for the technical surface, and `Aegisy Client` for the shipping desktop
application. Candidate public names must not imply upstream Codex/Claude ownership,
protocol compatibility, finished write autonomy, or a stable feature set that is not
delivered.

## Proposed Naming Structure

Use `Aegisy Coding` as the internal umbrella until Product, Brand, and Legal approve
the public name. Retain the literal user concepts `Chat` and `Work`. Use `Workbench`
only as a descriptive technical term in internal architecture and capability IDs,
not as a compatibility promise.

The final terminology decision must define one canonical Chinese and English term for
project, session, turn, task, runtime, workspace/root, proposal/change, approval,
checkpoint, and model profile. Wire identifiers remain versioned technical names and
do not change solely for marketing.

## Acceptance Criteria

- Trademark/domain/store-listing review and separation from OpenAI, Anthropic, Kimi,
  ACP, Codex, Claude, and other vendor brands.
- User research for Chinese and English comprehension of Chat versus Work and the
  read-only versus write-capable boundary.
- Consistent installer, window, updater, website, documentation, telemetry category,
  support, and accessibility names.
- Migration plan for `Aegisy Client` settings, bundle IDs, app IDs, shortcuts, and
  update feeds without breaking installed users.

## Consequences

No code or documentation may treat a candidate name as legally approved. Renaming the
surface cannot broaden protocol compatibility or mutation authority. OpenSpec `1.2`
remains the decision gate; this ADR only supplies ownership, constraints, and due
evidence.
