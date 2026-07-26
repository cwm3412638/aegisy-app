# Aegisy Architecture Decision Register

This directory owns the architecture questions listed in
`openspec/changes/build-aegisy-agent-workbench/design.md`. The task text for
OpenSpec `1.3` predates two additions and says seven questions; the design now
contains nine. All nine are registered here so none can disappear through wording
drift.

An ADR status has precise meaning:

- `Accepted`: implementation may rely on the decision, subject to its release gates.
- `Provisional`: current foundations may follow it, but the named evidence gate can
  still reverse it without compatibility promises.
- `Proposed`: an owner and deadline exist, but dependent product behavior stays off.
- `Deferred`: no implementation choice is authorized until the due gate is met.

| ADR | Question | Status | Accountable owner | Consulted owners | Due gate |
| --- | --- | --- | --- | --- | --- |
| [0001](0001-embedded-webengine-go-no-go.md) | Embedded Qt WebEngine viability | Provisional | Desktop Platform | Accessibility, Release Engineering, Product Security | OpenSpec `2.8`; stable release `22.7`/`22.8` |
| [0002](0002-codex-distribution-and-client-identity.md) | Codex distribution and enterprise identity | Proposed | Runtime Integrations | Legal, Release Engineering, Enterprise Security | `1.5`, `7.1`, before `22.4` |
| [0003](0003-acp-extension-policy.md) | Standard ACP versus Aegisy extensions | Accepted | Protocol Working Group | Runtime Integrations, Desktop Platform, Product Security | Recheck at `8.1`; approve each extension at `8.4` |
| [0004](0004-model-catalog-trust-and-authority.md) | Model catalog schema, trust, and field authority | Provisional | Model Control Plane | Product Security, Runtime Integrations, Billing, Desktop Platform | `9.1`-`9.4`, `9.7`, `9.10`, and `10.1` before selection/routing/token issue |
| [0005](0005-local-model-provider-policy.md) | Ollama/LM Studio policy | Proposed | Product | Model Runtime, Product Security, Privacy, Support | Dedicated local-provider OpenSpec before implementation; `10.11` is supplemental only |
| [0006](0006-windows-native-sandbox.md) | Native Windows sandbox selection | Deferred | Product Security | Windows Runtime, Release Engineering, Enterprise Security | `18.10`, release block `18.12` |
| [0007](0007-local-content-retention.md) | Terminal/diff/image/session retention | Provisional | Data Governance | Product Security, Product, Support, Enterprise Administration | Dedicated enterprise-retention OpenSpec before beta mutation; `5.8` and `22.9` remain prerequisites |
| [0008](0008-editor-language-intelligence.md) | LSP in the first editor milestone | Accepted | Editor Platform | Runtime Integrations, Product Security, Performance | Recheck when supported-language scope changes |
| [0009](0009-public-product-name.md) | Public product name and compatibility signal | Proposed | Product | Brand, Legal, Documentation, Support | OpenSpec `1.2`, before public beta and `23.1` |

Closing an ADR requires updating its file, this index, the relevant OpenSpec task,
and `PROJECT-MEMORY.md` when the decision changes architecture, security, packaging,
release, or persistent configuration. A due date is expressed as a repository gate,
not a calendar promise that could silently expire.
