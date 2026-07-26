# Aegisy Third-Party Component Inventory

Status: engineering inventory for product, security, packaging, and legal review.
This document records repository-selected components and integration ownership; it
is not legal approval. OpenSpec `1.5` owns legal review and `22.4` owns complete
release NOTICE/license packaging.

## Inventory Rules

- `Bundled` means bytes are copied or compiled into an Aegisy release artifact.
- `External` means Aegisy discovers a user/system installation and does not
  redistribute it today.
- `Planned` means no dependency or redistribution choice has been approved.
- Exact versions come from repository pins. A developer-machine installation is
  diagnostic state and cannot silently replace a repository pin.
- Before a component changes from External or Planned to Bundled, the release
  owner must record its exact artifact/hash, source, license texts, notices,
  architecture, update/rollback behavior, and security review.

## Agent And Protocol Components

| Component | Repository version | Status | License | Source of truth | Release requirement |
| --- | --- | --- | --- | --- | --- |
| OpenAI Codex CLI/App Server | `codex-cli 0.144.5` | External, exact-version runtime gate | Apache-2.0 | `agent-runtime/crates/aegisy-agentd/src/codex_adapter.rs`; npm package metadata | Current packages must not claim Codex is bundled. Bundling requires the exact package/platform artifact, Apache-2.0 text/NOTICE, transitive/native inventory, signature/hash, and updater compatibility evidence. |
| Aegisy Agent Protocol | `0.1` | First-party | Apache-2.0 | `agent-runtime/Cargo.toml`; `agent-runtime/aap-schema/stable/v0.1/` | Preserve Aegisy copyright/license and stable schema compatibility evidence. |
| ACP adapter/client | not selected | Planned, not integrated | not applicable yet | OpenSpec section 8 | Select an exact protocol/client implementation and license before code or distribution; no current package may imply ACP-agent compatibility. |

## Embedded Workbench Web Components

| Component | Exact version | Status | License | Source of truth | Release requirement |
| --- | --- | --- | --- | --- | --- |
| Monaco Editor | `0.55.1` | Bundled local web assets | MIT | `workbench-web/package.json` and lockfile | Include MIT license and generated-bundle component/version record. Remote loading remains forbidden. |
| xterm.js | `6.0.0` | Bundled local web assets | MIT | `workbench-web/package.json` and lockfile | Include MIT license and generated-bundle component/version record. |
| xterm.js FitAddon | `0.11.0` | Bundled local web assets | MIT | `workbench-web/package.json` and lockfile | Include MIT license with xterm.js inventory. |
| esbuild | `0.28.1` | Build-time tool, not runtime-loaded | MIT | `workbench-web/package.json` and lockfile | Record build provenance; do not present it as an application runtime capability. |

## Repository Parsing And Diff Components

The following Rust packages are compiled into `aegisy-agentd`. Versions and
licenses are verified from `agent-runtime/Cargo.lock` through Cargo package
metadata; `agent-runtime/THIRD-PARTY-NOTICES.md` records their runtime purpose.

| Component | Exact version | License |
| --- | --- | --- |
| tree-sitter Rust bindings | `0.26.11` | MIT |
| tree-sitter-cpp grammar | `0.23.4` | MIT |
| tree-sitter-javascript grammar | `0.25.0` | MIT |
| tree-sitter-python grammar | `0.25.0` | MIT |
| tree-sitter-rust grammar | `0.24.2` | MIT |
| tree-sitter-typescript/TSX grammars | `0.23.2` | MIT |
| similar | `3.1.1` | Apache-2.0 |

Release packaging must include the applicable license/copyright text for each
grammar, not only the tree-sitter binding license.

## Language Servers

Language servers are discovered as external executables and are not downloaded
or bundled by Aegisy. The current product does not pin their installed versions.

| Server family | Discovery name | Status | Upstream license family | Bundling gate |
| --- | --- | --- | --- | --- |
| rust-analyzer | `rust-analyzer` | External | MIT OR Apache-2.0 | Pin a tested distribution/version and its complete notices before bundling. |
| Pyright | `pyright-langserver` | External | MIT | Pin Pyright, Node/runtime ownership, and notices before bundling. |
| TypeScript Language Server | `typescript-language-server` | External | Apache-2.0 | Inventory both the server and TypeScript runtime/compiler packages before bundling. |
| clangd | `clangd` | External | Apache-2.0 WITH LLVM-exception | Pin the platform LLVM distribution and retain its LLVM exception/notices before bundling. |

An installed binary being detected does not make its behavior, version, or license
part of the signed Aegisy artifact. The UI must continue to report unavailable or
external state truthfully.

## Terminal, Cryptography, Storage, And Sandbox Boundary

| Component | Exact version/status | Integration | License | Boundary |
| --- | --- | --- | --- | --- |
| portable-pty | `0.9.0` | Compiled into sidecar for macOS PTY/Windows ConPTY | MIT | Terminal/process transport only; it is not an Agent sandbox or permission authority. |
| windows-sys | `0.61.2` | Compiled into Windows sidecar | MIT OR Apache-2.0 | Windows Job Object/process ownership only. |
| libc | `0.2.174` | Compiled on Unix targets | MIT OR Apache-2.0 | Process-group signalling only. |
| ed25519-dalek | `2.1.1` | Compiled into sidecar | BSD-3-Clause | Model-catalog signature verification; include BSD notice. |
| rusqlite | `0.40.1` | Compiled into sidecar | MIT | Workbench Store binding; inventory the selected SQLite linkage and public-domain notice separately in release output. |
| strip-ansi-escapes | `0.2.1` | Compiled into sidecar | Apache-2.0 OR MIT | Terminal excerpt normalization; include selected license notice and transitive `vte` notice. |
| OS Agent sandbox | no Aegisy dependency selected | Planned | not applicable yet | Current Codex integration remains `sandbox=read-only`, `approvalPolicy=never`. No package may claim a production Aegisy sandbox until an exact implementation and platform evidence are approved. |

## Updater And Native UI Support

| Component | Exact version/status | Distribution | License | Required evidence |
| --- | --- | --- | --- | --- |
| Sparkle | `2.9.4` | Bundled in macOS app | MIT plus embedded third-party notices | Preserve the complete upstream license file from the pinned archive and signed update evidence. |
| WinSparkle | `0.9.3` | Bundled in Windows app | MIT | Preserve upstream license/notices with the DLL and signed Windows evidence. |
| Lucide static icons | `1.24.0` | Bundled Qt resources | ISC | `assets/icons/lucide/LICENSE` is the checked-in license source. |
| Qt | build-selected Qt 5.15+ or Qt 6 | Bundled/deployed native libraries | distribution-dependent LGPL/GPL/commercial terms | Release owner must record exact Qt version/modules, chosen license basis, relinking/source obligations where applicable, and WebEngine notices. |
| OpenSSL | build-selected OpenSSL 3 on current release paths | Bundled on Windows; linked per platform | Apache-2.0 | Record exact build/runtime versions and DLL/framework hashes; Windows TLS verifier must reject mixed roots. |

## Release And Review Gates

1. `agent-runtime/Cargo.lock` and `workbench-web/package-lock.json` are the exact
   dependency version authorities for their respective artifacts.
2. `agent-runtime/THIRD-PARTY-NOTICES.md` must remain synchronized with direct
   runtime-purpose changes. A generated release inventory must additionally include
   all applicable transitive license texts from the locked packages.
3. Signed macOS and Windows packages must carry an inspectable NOTICE/license set
   matching the actual bundle, including Qt WebEngine, updater, web assets, sidecar,
   and any future pinned adapter.
4. Source availability, attribution, modification notices, relinking requirements,
   export/cryptography review, trademarks, and product naming are legal-review
   decisions under OpenSpec `1.5`; this engineering inventory does not approve them.
5. Unknown, unpinned, missing-license, or mismatched components block promotion.
   They must not be omitted from the inventory or inferred safe from a successful
   build.
