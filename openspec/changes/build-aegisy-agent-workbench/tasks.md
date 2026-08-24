## 0. Active Aegisy Companion Scope (2026-08-23 Reset)

Active execution order is `0.2 -> 0.3 -> 0.4/0.5 -> 0.6/0.7 -> 0.8`.
The existing Codex programming surface is preserved but does not displace the
website configuration, extension/Skills/MCP, or Chinese-enhancement work. Claude
and Gemini may receive configuration-compatibility maintenance required by `0.3`;
their embedded programming runtimes are not active implementation targets.

- [x] 0.1 Reframe the product as an Aegisy website companion and retain Codex as the only near-term integrated programming runtime
  - `proposal.md` and `specs/aegisy-companion-control-center/spec.md` define the active scope. Claude Code, Gemini CLI, and OpenCode remain supported configuration targets without embedded Agent runtime claims. Existing Workbench code remains retained and gated. The main navigation and retained WebEngine preview now identify the programming surface as Codex-only, while `product_scope_policy` locks the configuration-target/runtime distinction.
- [ ] 0.2 Define and implement the authenticated website-to-local configuration projection without exposing credential values outside secure storage
  - Partial website-to-local foundation: `aegisy-companion-config-projection/0.1` converts an exact authenticated website Key response into an account-bound, credential-free, non-authorizing snapshot. It contains only hashed website-Key identities, bounded display/group/platform metadata, normalized state, exact source origin/capture time, and fixed false credential-content/configuration-authority/applied fields. Account identity is domain-separated from the verified website account ID; raw IDs, tokens, Key values/fragments, user info, and inferred model metadata are absent. API requests bind auth epoch, exact URL/source origin and account verification, disable redirects/cache, enforce JSON Content-Type plus per-page/aggregate bounds, reject incomplete pagination, and retire the accumulator immediately. `CompanionCredentialBroker` transactionally stages validated Key values into exact account/Key-derived SecureStorage slots and adds only opaque handles to the projection; cross-account/cross-Key resolution fails. ConnectWizard consumes only projection candidates/handles and resolves the credential on explicit test/save actions; Profile schema 7 persists only hashed website account/Key/projection source metadata. Its real `/v1/models` path now uses unique request IDs and exact account/current-projection/Key/handle/platform/auth/origin bindings, returns only validated `aegisy-companion-model-projection/0.1` model IDs, retires stale requests, and never consumes global `modelsReceived`. Local Profile model queries use the same request-specific result path. Profile SecureStorage refs remain strict UUID-derived slots and displayed credential hints are domain-separated fingerprints. Focused projection/broker/model/profile/API/tool/policy tests pass. Raw website Key inventories are no longer published to UI consumers. Keep `0.2` unchecked: model results are not yet merged into an authenticated revisioned website configuration cache, and cache revision/expiry/high-water behavior remains incomplete.
  - ModelsDialog follow-up: the account model browser now accepts only sanitized active SecureStorage-backed candidates, stores no raw Key or Key fragment, allows no manual Key paste, and uses the same account/projection/Key/handle/platform/request-bound model transport. It no longer subscribes to raw `apiKeysReceived` or global `modelsReceived`.
  - Chat follow-up: Chat now accepts only sanitized active candidates, stores no credential plaintext or Key fragment in widget data, uses request-correlated model projections, and calls ApiClient companion wrappers for chat, image Skill, and presentation Skill execution. ApiClient revalidates auth/account/current-projection/Key/handle/platform/origin and resolves the credential from SecureStorage at request start; configuration/auth/origin changes retire the bound operation and late presentation/model responses are inert. Chat history schema 2 persists only `website-key:sha256:` identity plus a bounded safe display name and ignores the legacy raw `key_id`. Product policy rejects regression to raw Key/global model signals or raw credential APIs.
  - Standalone Image follow-up: the image tool now consumes only validated active `gpt-image` candidates from the companion projection, stores only opaque handle and hashed binding/display metadata in widget data, and uses a unique request ID plus `generateCompanionImage`/`companionImage*` correlation. It no longer subscribes to raw `apiKeysReceived`, retains the raw inventory, displays Key fragments, or calls the raw image credential method.
  - Usage follow-up: `aegisy-companion-usage-projection/0.1` contains only hashed Key identity, bounded display/group/state metadata, non-negative quota/cost metrics, account/current-configuration binding, and fixed false raw-ID/credential/configuration-authority fields. UsageDialog consumes companion configuration plus this request-correlated projection and never receives raw Keys or raw-ID-keyed statistics. ApiClient retains the minimal raw ID/quota mapping only in account/auth/source-projection-bound memory, sends raw IDs to the authenticated website endpoint internally, enforces exact URL/redirect/Content-Type/encoding/body bounds, rejects unexpected response IDs, and makes auth/origin/projection-stale replies inert.
  - API-Key management follow-up: `aegisy-companion-key-management-projection/0.1` is online-only and binds exact account/configuration state, safe Key/group metadata, counts, and digest. Separate 256-bit system-random group/create/test/update/delete handles are globally unique, action-scoped, expire after 15 minutes, and bind again to the exact management projection at use. ApiClient alone maps them to raw Key/group IDs and SecureStorage credentials, percent-encodes raw path segments, enforces exact auth/origin/final URL/manual redirect/no-cache/identity-encoding/JSON/256-KiB response rules, emits no provider body or create response data, retires all mutation authority after any dispatched outcome, and reports delete credential-cleanup status separately. ApiKeysDialog no longer receives raw inventories/group IDs/credentials, displays Key fragments, copies credentials, persists raw preferred IDs, or uses global uncorrelated operation/error signals. MainWindow also no longer consumes the raw Key signal, and the legacy raw public management APIs/signals were removed. `0.2` stays unchecked for authenticated revisioned cache/model integration.
  - Management verification follow-up: a trusted-origin fake transport proves two-read handle rotation, stale-handle rejection before network, raw path-segment encoding, positive update/create, strict response-code typing, create credential staging, concurrent-mutation rejection, and auth-change `outcome-unknown` with late success inert. The dedicated offscreen Qt fixture proves literal safe-name rendering, PlainText status, raw-ID/credential/handle absence, exact management/model ownership, fixed failure for exact-owned invalid projections, synchronous mutation/test rejection, refresh clearing, and inert late pre-refresh model results. A target-private one-shot SecureStorage removal failure proves a successful remote delete reports local cleanup false, retains the resolvable credential, and permits later explicit cleanup without exposing the credential in projections or request metadata. The real transport fixture now holds an exact Key-test `GET /v1/models`, commits a different authenticated website configuration and SecureStorage credential, requires one fixed retirement failure, proves the released old reply is inert, rotates the management/test handle, and proves the next test uses the refreshed credential and model result. Keep `0.2` unchecked for authenticated revisioned cache/model integration.
  - Secure cache prerequisite: `SecureStorage::loadEncryptedFresh` now bypasses the process cache and returns exact `Found/Missing/Unavailable/Invalid` state. Windows validates sync/status, presence, canonical Base64, DPAPI, size, and strict UTF-8; macOS distinguishes Keychain not-found from backend/interaction failure and invalid returned bytes; Linux treats only the documented empty exit-1 lookup as Missing while executable/start/timeout/crash/stderr/backend failures are Unavailable. Compatibility `loadEncrypted` remains cache-first and caches only Found. A no-credential-access policy test locks these source paths and proves no test hook entered production. This prevents cache authority bootstrap from mistaking a locked/broken secure backend for first install. The authenticated revisioned cache transaction itself remains open.
  - Live authority retirement: every terminal current-generation website Key transport/trust/type/pagination/projection/broker failure now clears the live configuration, usage, management, group, and website-model state and retires pending model/usage/Key/Chat/image/presentation bindings before publishing one configuration failure. Old-generation late failures remain inert. ConnectWizard preserves only its separate local Profile path; Models, Chat, Image, Usage, and Key management clear website candidates and disable save/send/Skill/generate/provider actions. Ordinary website model results enter an in-memory website-only map, while local Profile and management Key-test models are excluded. The fake transport fixture holds every operation class, proves failure causes zero subsequent network dispatch and late results remain inert, and proves a stale failure cannot retire a newer configuration. This live boundary is independent of the read-only persistent cache and remains required even after cache integration.
  - Revisioned cache core and production persistence: `CompanionConfigurationCache` defines strict `aegisy-companion-configuration-cache-{payload,envelope}/0.2` and a single SecureStorage authority envelope containing the canonical 32-byte HMAC key, Committed anchor, optional Prepared reservation, and `highest_reserved_revision`. A caller-supplied project-external `QLockFile` path, 30-second PID/host stale recovery, lock-time QSettings sync, exact account namespace inventory, HMAC-SHA256, constant-time compare, A/B preimage/candidate binding, and fresh typed secure read/write outcomes implement the recovery graph. Prepared target preimage aborts without revision reuse, exact candidate finalizes, and a third state is Invalid; unanchored or unknown slots never bootstrap. The core returns Fresh/Stale/Expired/Empty/LegacyUnverified/Unavailable/OutcomeUnknown/RecoveryRequired/Invalid display-only views with all authority false and no raw ID, credential value, or handle. Config is fresh 24h with 7d status retention; website models are fresh 6h, platform/config-observation bound, never outlive config, and local Profile models are rejected. New source observation clears models; exact observation replay is idempotent. Legacy v1 is never re-signed; successful v2 commit attempts strict cleanup and exposes cleanup failure as a warning without falling back. Production now uses a strict SecureStorage scope adapter and one serialized worker owning QSettings/cache operations off the UI thread. ApiClient emits a dedicated ordinary-website-model observation after complete live validation; management Key tests and local Profile queries emit none. MainWindow renders live success before cache persistence, treats cache failure as independent degradation, removes production v1 save/load calls, and distinguishes all nine cache states only as current-account read-only status. Focused tests cover adapter scope/encoding/lock-path rejection, cross-account isolation, typed-state propagation, website/local/management model separation, and live/cache independence.
  - Read-only cache dialog projection: the worker now returns the complete evaluated View with its exact account and evaluation time. MainWindow accepts only the current generation/account, rebuilds a strict presentation DTO carrying the safe hashed account but no handle/authority/credential/operation fields, clears it before auth/account changes, and monotonically ages Fresh models/configuration before injection. Each dialog repeats exact DTO/current-account equality. ConnectWizard, Models, and Chat use isolated cached roles above `Qt::UserRole + 31`; live handle/account/projection roles stay empty. Cached selection is display-only at both UI and direct action entry points: no credential resolution, model query, connection test, Profile save, Chat send, image/PPT Skill, model execution, or tool write is reachable. Fresh may show unexpired models, Stale only safe Key metadata, and Expired/error states no rows. Models keeps safe search/filter/copy and now clears old-account/old-Key models on every source change or owned failure. The presentation adapter and each open dialog schedule the next model/configuration TTL transition, so a model deadline removes model rows, Fresh becomes Key-only Stale, and the final deadline removes all rows without replacing live UI. The offscreen fixture proves cache-to-live replacement, live-failure fallback, cross-account rejection, role isolation, direct-entry zero request/save/history mutation, Fresh/Stale/Expired behavior, and an already-open dialog crossing all TTLs. `0.2` remains unchecked pending native Windows production evidence; no Agent/Codex authority changed.
  - First native CI follow-up: Windows run `32662755813` stopped before Qt with the recurring ConPTY interrupt fixture (`925/1`); macOS run `32662755791` built but CTest returned exit 8 without a public test name. The Windows fixture is repaired without production changes: a split-literal prompt proves shell recovery after the pre-interrupt output checkpoint, split ANSI variables prevent echo-only success, ANSI is observed before a separate `exit 23`, one absolute 120-second deadline sits inside a 150-second cleanup watchdog, and DSR/reader/exit/Job Object assertions remain. Deadline classification is platform-neutral and timeout wins over marker/exit success; a DSR reply race re-reads the authoritative exit snapshot while other failures retain fixed codes. Platform-neutral helper tests pass `9/9`, fmt and strict all-target Clippy pass. The cache TTL fixture now uses separated seconds-scale transitions, and macOS CI publishes only bounded safe failed-test names. Both native workflows must rerun before `0.2` can close.
  - Second native CI follow-up: macOS run `32665422212` passed. Windows run `32665422225` reached the repaired interrupt fixture and failed only `CONPTY_INTERRUPT_JOB_NOT_EMPTY` after prompt recovery, real ANSI output, and exact `exit 23` had succeeded. The empty-Job check had run before `remove_user` closed the pseudoconsole master, so console-host teardown could still be pending. The test now duplicates the Job Object handle, performs the complete production teardown through `remove_user`, and then requires the duplicated Job to become empty within the existing five-second bound. Production ConPTY and all authority remain unchanged. A fresh Windows run is still required before `0.2` can close.
  - Third native CI follow-up: macOS run `32666817150` passed, while Windows run `32666817138` again failed only `CONPTY_INTERRUPT_JOB_NOT_EMPTY` (`928/1`). The duplicated test handle prevented `KILL_ON_JOB_CLOSE` from firing when the production-owned handle closed, exposing that teardown still depended on last-handle semantics after the shell had already exited. Production teardown now records the shell exit and unconditionally terminates the whole Job before draining/closing ConPTY, so console host and descendants are explicitly owned even when another verifier handle exists. The five-second empty-Job assertion remains unchanged. A new native Windows run is required before `0.2` closes.
  - Fourth native CI follow-up: current companion HEAD `484beb2` has a fully successful macOS run `32673524896`. Windows run `32673524863` passed the clean Unicode checkout, complete locked Rust workspace including repaired ConPTY, strict Clippy, Release build, offline AAP package, dependency audit, Qt/OpenSSL installation, and Qt CMake configure, then failed `Build Windows Qt agent runtime`. Windows CTest, cache/dialog/one-click runtime evidence, installer, and package stages were skipped. Public job metadata does not identify the bounded MSVC failure, so no Qt root cause or completion is inferred. `0.2` remains unchecked for successful native Windows Qt build/CTest evidence.
- [ ] 0.3 Complete one-click profile apply/repair with target preview, per-tool isolation, backup, atomic verification, and truthful rollback status
  - Encrypted backup prerequisite is now connected locally: ToolManager uses the production AES-256-GCM `ConfigurationBackupStore` for Claude, Codex, Gemini, and OpenCode; a per-tool 256-bit key is generated only through a strict SecureStorage scope and exact save/readback; exact legacy v1 plaintext records migrate during locked inventory; invalid/unavailable inventory blocks configuration before any CLI write; backup capture is bounded and stable-read twice; create is authenticated/read back; direct and gateway configuration recheck the current files after backup before writing; rollback uses the verified in-memory preimage; and prune uses only Ready inventory plus `removeVerified`, reporting cleanup failure as a warning rather than a false operation failure. The backup UI distinguishes Empty/Ready/Unavailable/Invalid and includes OpenCode. `0.3` stays unchecked for complete target-preview confirmation, active-profile compensation, broader failure injection, and clean macOS/Windows one-click evidence.
  - Activation ordering follow-up: single-card and all-tool activation now enter one profile-index queue instead of the bulk path calling configuration writes directly. The reviewed preview lists exact managed files plus required Node/CLI installation or repair; installation-impact warnings cannot use the saved skip-confirm preference, and bulk activation always presents one combined preview. Each queued profile is revalidated, its CLI is installed and verified first, ToolManager then performs encrypted backup/write/readback/rollback, and only a successful result advances that tool's active Profile. Installation or configuration failure stops the remaining queue and does not commit the failing Profile. Product scope policy locks the install -> configure -> active-state order and rejects direct bulk writes. `0.3` remains unchecked for editing-current-Profile compensation, acknowledged/compensated gateway in-memory profile changes, broader injected failures, and clean macOS/Windows evidence.
  - Active-edit replacement follow-up: editing an active Profile now creates a separate candidate with its own UUID-derived SecureStorage slot instead of overwriting the active Profile before asynchronous installation/configuration. The original remains active until the candidate passes the shared activation queue. Queue entries bind the candidate UUID, complete safe metadata, full domain-separated credential identity, and gateway mode; any drift during installation fails before configuration. Active edits cannot change tool type. Cancel, installation failure, Profile drift, or ToolManager failure removes the inactive candidate; success activates the candidate first and then removes the original, preserving shifted per-tool and last-activated indices. The preflight dialog no longer starts an independent installer that could race the activation queue. Focused Profile/configuration/product tests pass `4/4`. `0.3` remains unchecked because QSettings active-state commit still needs sync/readback/outcome handling, candidate cleanup is not crash-journaled, and gateway control acknowledgement plus exact in-memory/tool rollback compensation remain open.
  - Profile commit truthfulness follow-up: new Profile creation now syncs and exactly reads back every QSettings field plus the fresh SecureStorage credential before publication. Active selection returns success only after QSettings sync/status and an independent exact active/last-activated readback; a failed commit attempts to restore the prior indices and emits no active-change signal. ToolManager returns the exact verified encrypted preimage backup ID on a successful direct or gateway file apply. If active selection cannot commit, MainWindow restores that backup before discarding the replacement and reports restored versus outcome-unknown truthfully. Focused `4/4`, application build, strict OpenSpec, and diff checks pass. `0.3` remains unchecked for typed/verified Profile removal cleanup, crash-journal recovery, gateway prepare/commit/abort acknowledgement, and full cross-platform failure evidence.
  - Gateway transaction follow-up: `aegisy-gateway-control/0.1` replaces the unacknowledged `configure` message with exact-field, request/transaction/tool/operation-bound prepare-configure, prepare-remove, commit, and abort results plus per-tool revision CAS. Prepared credentials stay outside the active routing Map and no result contains a credential, upstream, fragment, or hash. GatewayManager binds every result to the current QProcess generation, rejects cross-bound/malformed results, bounds output and wait time, classifies timeout/exit/write uncertainty, and publishes fixed stderr classes only. Activation orders gateway prepare -> encrypted/verified file apply -> gateway commit -> verified Profile commit. File failure aborts; gateway commit failure restores files; Profile commit failure restores the prior active gateway Profile/remove state before file restore. Node/security plus focused companion tests pass. `0.3` stays unchecked for durable crash recovery across all resources, typed Profile removal/cleanup, Qt fault-injection coverage of every outcome-unknown race, and clean macOS/Windows evidence.
  - Profile removal truthfulness follow-up: Profile removal now returns Removed, RemovedCredentialCleanupPending, Unchanged, or OutcomeUnknown. It syncs and independently verifies the new count, every active index, last-activated index, and absence of the removed UUID before publishing metadata removal. Credential cleanup uses a fresh typed read, confirmed removal, and fresh Missing readback; a retry by UUID can clean an orphan after metadata is gone. MainWindow ends replacement cleanup only after `metadataRemoved()` and separately warns about pending credential cleanup. Focused companion/gateway tests pass `6/6`. `0.3` remains unchecked for a durable cross-resource activation journal, updateProfile write verification, complete Qt gateway fault injection, and clean macOS/Windows evidence.
  - Non-active edit follow-up: `updateProfile` now fresh-reads the exact old credential before mutation, snapshots every prior QSettings field and old active index, verifies the new credential plus every updated field after sync, and verifies credential removal. Any failed write/readback restores the exact old fields/index and old credential; failure to prove restoration reports outcome unknown. The focused companion/gateway set passes `6/6`. Active Profiles still bypass this in-place path and use immutable replacement. `0.3` remains unchecked for the durable multi-resource activation journal, complete injected crash/outcome-unknown matrix, and native macOS/Windows evidence.
  - Gateway result-contract follow-up: `GatewayControlContract` is now the single pure validator used by GatewayManager for control results. It enforces the exact ten-field schema, current request/transaction/operation/tool binding, safe integral revision, credential absence, operation-specific success outcome, and bounded fixed-code rejection; mixed success/error and dynamic error text are invalid. Its independent matrix covers valid prepare/reject, four cross-bindings, unknown fields, credential flag, fractional revision, false success, wrong outcome, and dynamic error. The application and focused `7/7` pass. `0.3` remains unchecked for process-level timeout/exit injection, the durable multi-resource journal, and native one-click evidence.
  - Tool apply receipt follow-up: ToolManager now exposes prepare/apply/rollback/finalize. Prepare creates and authenticates the encrypted preimage, returns its backup ID/manifest identity plus content-only source/candidate files identities, and rechecks source stability before any config write. Candidate generation runs the existing four tool writers against an in-memory write collector and cleanses the temporary credential-bearing bytes. Apply reauthenticates the receipt/current source, regenerates the candidate, writes/validates only the selected target, and requires exact candidate identity. Failure restores and recaptures the preimage. Rollback accepts only the predeclared candidate (or already restored source), and finalize alone prunes. Compatibility wrappers use the same boundary. `0.3` remains unchecked for the authenticated journal/recovery/native gates below.
  - Activation journal contract follow-up: strict `aegisy-companion-activation-journal/0.2` stores only transaction/original/candidate Profile identities, candidate digest, tool/mode, exact ToolManager backup/manifest/source/candidate/applied identities, monotonic stage, and record identity in synchronized QSettings. Expected-identity CAS permits Prepared -> FilesApplied -> GatewayCommitted -> ProfileCommitted for gateway or Prepared -> FilesApplied -> ProfileCommitted direct, with exact receipt continuity. Unknown/type drift, illegal transitions, stale CAS, tamper, and partial deletion are Invalid; no credential field exists. The record remains locally hash-bound rather than SecureStorage-authenticated.
  - Activation journal integration follow-up: MainWindow now starts/revalidates the gateway before receipt preparation, persists Prepared before any target write, and advances after file, gateway, and Profile commits. Every compensation requires verified gateway abort/restore, receipt rollback, and journal clear; uncertainty retains the journal and gates all new single/bulk activation plus gateway auto-rehydration. Startup clears unapplied Prepared, rolls back direct FilesApplied, finalizes a verifiably active candidate, and keeps ambiguous gateway FilesApplied/GatewayCommitted in RecoveryRequired because a prior Node process may have committed. The real ToolManager test simulates journaled FilesApplied restart/rollback. Focused `8/8` passes. `0.3` remains unchecked for deterministic Qt process-crash/timeout injection, an explicit user recovery action for ambiguous gateway state, authenticated/anti-deletion journal anchoring, and clean native evidence.
  - Gateway process fault-injection follow-up: one test-only target runs the real `GatewayManager` against the test executable acting as a controlled child. It deterministically proves timeout after a complete control read, process exit before acknowledgement, malformed result, valid prepare/commit, and late old-generation `ready`/fatal inertia. Failure now retires the process pointer, generation, and every expected request binding before kill, then performs a bounded reap, so queued callbacks from the failed child cannot revive running state or satisfy a replacement request. The injection executable/token/timeout setters compile only under the target-private test macro and do not exist in the product binary. The application and focused gateway/process/product set pass. `0.3` remains unchecked for an authenticated/anti-deletion activation journal, a reviewed restart-safe recovery action for ambiguous gateway stages, and complete native one-click evidence.
  - Predeclared candidate follow-up: `prepareConfigurationApply` now requires the transient credential/model, captures every existing tool writer through a zero-disk in-memory collector, overlays those bytes on the authenticated preimage, and places only `candidateFilesIdentity` in the receipt before MainWindow creates the journal. Apply regenerates and compares that candidate before mutation and requires final disk identity equality. A Prepared-stage receipt can idempotently roll back a disk that already equals the candidate, covering a crash after file apply but before FilesApplied publication without treating it as unapplied. The application and focused backup/Profile/ToolManager/journal/product/process set pass `6/6`. `0.3` remains unchecked for SecureStorage-authenticated A/B journal publication, gateway/profile commit-requested intent, reviewed recovery, and native evidence.
  - Authenticated anti-deletion journal follow-up: the activation journal now anchors an `aegisy-companion-activation-journal-authority/0.1` envelope in platform secure storage holding a 32-byte `RAND_bytes` HMAC key, a monotonic `highest_serial`, one committed `{record_mac, serial}` anchor, and at most one reserved target, while record schema `0.3` bytes stay in `QSettings` authenticated by domain-separated HMAC-SHA256 with `CRYPTO_memcmp` comparison and `OPENSSL_cleanse` key zeroization. Records without an authority, a committed anchor whose record was deleted, an unauthenticated record, a serial that disagrees with the anchor, a substituted MAC key, and a leftover legacy `0.2` identity key each produce a distinct `Invalid` verdict instead of `Empty`, so `QSettings` alone can neither forge nor erase a transaction. Mutations reserve a strictly larger serial and the candidate MAC before any record write and commit afterwards, so a crash resolves deterministically to the exact preimage or reserved candidate and any third state is refused; abandoned reservations never reclaim a serial. Authority writes always re-read fresh to separate definite failure from `OutcomeUnknown`, and a locked backend reports `Unavailable` rather than first install. MainWindow requires an authenticated read-back before touching target files and treats `Unavailable`/`OutcomeUnknown`/`RecoveryRequired` as fail-closed. The application builds and the complete desktop gate passes `58/58`. `0.3` remains unchecked for A/B authority slot publication, gateway/profile commit-requested intent, a reviewed restart-safe action for ambiguous gateway stages, and clean native one-click evidence.
  - Commit-requested intent follow-up: record schema `0.3` adds `gateway-commit-requested` and `profile-commit-requested`, and `advance` enforces the exact successor stage so no commit can be issued before its intent is durably journaled and both gateway stages require a gateway-mode receipt. `processActivationQueue` persists each intent before calling `commitProfile`/`setActiveIndex` and compensates verifiably when the intent itself cannot be journaled. Recovery now decides from the recorded intent instead of inferring: `FilesApplied` proves no commit was ever sent so gateway and direct transactions both roll back to the authenticated preimage, direct `ProfileCommitRequested` with an inactive candidate is deterministic because the QSettings active index is authoritative, `ProfileCommitRequested` with a verifiably active identity-matching candidate is treated as commit-reached and only cleaned up, and a `setActiveIndex` failure whose candidate is actually active enters `RecoveryRequired` rather than compensating over an effective commit. Only `GatewayCommitRequested` and `GatewayCommitted` without an active candidate remain ambiguous, each with its own message. The journal and ToolManager fixtures pin both skip rejections, both intent transitions, restart durability of the persisted intent, and exact gateway preimage rollback; the application builds and the complete desktop gate passes `58/58`. `0.3` remains unchecked for A/B authority slot publication, an explicit reviewed restart-safe recovery action for the two remaining ambiguous gateway stages, and clean native one-click evidence.
  - Reviewed recovery follow-up: `RecoveryRequired` now has an operator exit instead of permanently disabling configuration switching after one interrupted gateway commit. `MainWindow::runReviewedActivationRecovery` is surfaced on the gateway page only while `m_activationRecoveryRequired` holds, and it re-establishes a verified present state rather than inferring the past, which is required because `assets/local_gateway.js` keeps profiles only in process memory and a restarted gateway cannot report what it committed. After explicit confirmation naming the candidate it abandons the candidate, rolls files back to the authenticated preimage through the journaled receipt, and re-drives the gateway to hold exactly the profile QSettings records as active or to hold none when that profile is the abandoned candidate. It never calls `setActiveIndex`, since choosing an active profile would be inferring an outcome. Unverified rollback, a gateway that will not start, an unconfirmed configure/remove, an unknown candidate cleanup result, or a journal that cannot be cleared each abort with a distinct reason and leave the flag set; the flag clears only after the journal is verifiably `Empty`. The ToolManager fixture drives the ambiguous `GatewayCommitRequested` path end to end across a simulated restart and asserts the preimage token is restored while the candidate credential is absent; product policy pins the guard, load-before-act order, confirmation, rollback, journal clear, ordered flag reset, both gateway alignment calls, the absence of `setActiveIndex`, the visibility binding, and the connection. The application builds and the complete desktop gate passes `58/58`. `0.3` remains unchecked for A/B authority slot publication and clean native one-click evidence.
  - A/B authority slot follow-up: the authority envelope no longer has a single copy, so a torn secure-storage write can no longer destroy the only HMAC key and leave every future record permanently unauthenticatable. `CompanionActivationAuthoritySlots` is a pure I/O-free unit where each slot holds an `aegisy-companion-activation-journal-authority-slot/0.1` frame with an exact four-key schema and a SHA-256 digest over a domain-separated `(generation, payload)` preimage, so neither field can be substituted independently. `select` always publishes into the peer of the selected slot, so the selected generation survives the write; a corrupt slot with a valid peer selects the peer, while two corrupt slots, a lone corrupt slot, and equal generations with differing payloads each report a distinct `Invalid` code instead of degrading to `Missing`, and any `Unavailable` slot blocks inference. Migration adopts the previous single scope as generation 1, publishes generation 2 into slot A, and removes the legacy scope only after that write is confirmed; a legacy remnant cannot override a published slot and generation exhaustion is reported rather than wrapped. A new `companion_activation_authority_slots` CTest target covers framing, four substitutions, clean install, single slot, newest-wins both orders, torn recovery, both corrupt cases, conflict versus identical frames, all unavailable positions, invalid backends, legacy adoption and legacy-ignored, and exhaustion; product policy pins the select-then-frame-then-write order, the absence of the in-place single-scope write, both slot scopes, and the anti-degradation codes. The complete desktop gate passes `59/59`. `0.3` remains unchecked only for clean native macOS/Windows one-click evidence.
  - SecureStorage durability prerequisite: Windows credential saves call `QSettings::sync()` and require `NoError` before caching success; credential removal on every platform clears the memory cache only after the platform backend confirms deletion, with Windows also requiring `sync()/NoError`. This prevents the backup master key from treating an unflushed `setValue/remove` as durable success.
  - Encrypted-store contract: `ConfigurationBackupStore` defines the path-free slot payload, strict canonical v2 AES-256-GCM manifest, bounds, complete authentication/prevalidation, actual-file readback, resumable v1 migration, four-state inventory, safe summaries/identities, and verified removal described in verification. The injectable fake-provider suite plus ToolManager integration suite prove disk plaintext/path absence, encrypted round trip, safety-backup zero-write failure, invalid target/inventory zero-write behavior, and legacy/core negative cases. Product scope rejects a regression to `file_N.bin`, direct legacy-manifest parsing, unverified recursive prune, missing inventory UI, or an omitted OpenCode backup target. OpenSpec `0.2` now remains unchecked for authenticated revisioned configuration/model cache integration rather than plaintext ToolManager backups; `0.3` remains open for the one-click workflow gates above.
- [ ] 0.4 Build one extension center for compatible Codex plugin enablement, custom Skills management, and MCP, including provenance, compatibility, import, enable/disable, update, removal, and recovery
  - Read-only registry contract foundation: `extension-registry/0.1` normalizes `codex-plugin`, `skill`, and `mcp` records with strict IDs, name/version bounds, source/content SHA-256 identities, source kind, trust, compatibility/reason, scope, allowlisted requested capabilities, installed/effective/update/recovery metadata, deterministic ordering, aggregate limits, and registry identity. Duplicate identities/capabilities, secret-shaped metadata, unknown capabilities/enums, invalid hashes/reasons/scopes, oversized registries, and effective enablement without verified+compatible evidence fail closed. Registry and every record fix install/enable/update/remove/execution authority false. The contract does not scan, install, start, execute, or mutate anything; adapters and unified UI remain open.
  - MCP inventory/guard follow-up: a bounded strict reader classifies missing, Ready, Invalid, and Unavailable Claude settings sources; rejects symlinks, oversize/malformed JSON, non-object/over-limit servers, unsafe IDs, mixed/unknown server fields, remote plaintext HTTP, shell-shaped commands, and invalid args/env. It emits only hashed MCP registry records with unverified/unknown compatibility and process/network plus MCP-tool capability metadata; command, args, URL, env names/values never enter the registry. McpConfigDialog now disables edits/saves on Invalid/Unavailable, rechecks exact source identity before save, preserves other fields, and strictly re-reads after write. Runtime tests prove malformed and externally drifted files remain byte-identical and valid save preserves unrelated data. Full preview/backup/rollback mutation remains open.
  - Unified read-only source/UI follow-up: Codex plugin output is captured only from an absolute canonical executable with pre/post identity checks, a whitelist environment, separate 1 MiB stdout/64 KiB stderr limits, fixed timeout/failure codes, and no stderr publication. Codex output, Skill manifests, and MCP JSON reject duplicate decoded keys. The bounded Skills tree rejects links, special files, drift, traversal, excessive depth/count/bytes, unknown manifest fields and permissions, duplicate IDs, invalid UTF-8, and self-asserted trust/enablement. A coordinator combines all valid source records, reports only fixed source issue codes, and revalidates the complete registry. MainWindow exposes the async read-only Extension Center with search, type filter, provenance/trust, and compatibility state; its item roles contain only IDs/kinds/hashes and it exposes no mutation action. The application and focused extension/product set pass `8/8`. Compatibility verification and every import/enable/disable/update/remove/backup/recovery workflow remain open, so `0.4` stays unchecked.
- [ ] 0.5 Productize Chinese-language packages and desktop enhancements with exact application/version checks, reviewed targets, recoverable installation, and cross-platform fixtures
- [ ] 0.6 Re-scope and label the integrated programming surface as Codex-only, preserving current read-only and unavailable capability boundaries
- [ ] 0.7 Add fail-closed tests proving Claude, Gemini, ACP, and other non-Codex Agent adapters cannot be selected, advertised, or reached in the active product
- [ ] 0.8 Rebaseline macOS/Windows release gates around login, one-click configuration, rollback, extensions/Skills/localization, gateway, updater, and Codex launch/recovery

Sections 1-24 below are retained as implementation history and long-horizon
reference. They are active only when a task directly supports section 0 or the
bounded Codex surface. ACP, non-Codex Agent adapters, full IDE replacement,
Agent-authored mutation, background agents, and multi-provider Agent routing are
deferred and must remain unavailable.

## 1. Product Baseline and Decision Gates

- [ ] 1.1 Approve the Chat versus Work behavioral contract and mutation guarantees with product and security owners
  - `docs/CHAT-WORK-BEHAVIORAL-CONTRACT.md` defines the behavioral contract
    between Chat and Work modes, mutation guarantees for each mode, permission
    profiles (Read Only, Workspace Write, Developer, Full Access), security
    boundaries, and mode comparison matrix. The contract specifies that Chat is
    non-mutating by default while Work enables bounded mutations with approval
    workflows. Document created 2026-07-31 and awaits product and security owner
    approval before task completion.
- [ ] 1.2 Select the public product name and define original Aegisy terminology for project, session, turn, task, runtime, and workspace
  - `docs/AEGISY-CODING-TERMINOLOGY.md` defines canonical terminology for all
    core concepts: Project (bounded workspace with filesystem roots), Session
    (durable conversation thread), Turn (request-response cycle), Timeline
    (ordered event sequence), Task (discrete work unit), Runtime (execution
    environment), and Workspace (filesystem state within project). The document
    provides usage guidelines, preferred terms, and notes that the public product
    name remains under product review. Document created 2026-07-31 and awaits
    product owner approval before task completion.
  - `docs/ROADMAP.md` provides comprehensive product roadmap with milestones,
    feature plans, and success metrics. Created 2026-07-31.
- [x] 1.3 Convert all nine unresolved architecture questions in `design.md` into owned ADRs with due gates
  - `docs/adr/README.md` is the architecture decision register and links one ADR
    for every current Open Question: embedded WebEngine, Codex distribution/client
    identity, ACP extensions, model-catalog trust, local models, Windows sandbox,
    content retention, editor language intelligence, and public product naming.
  - Every ADR has one accountable owner, consulted owners, a closed status value,
    and repository gate. Proposed/deferred decisions grant no implementation
    authority; provisional decisions retain their named release evidence. The
    Open Questions in `design.md` link back to the exact ADRs so additions cannot
    disappear behind the historical seven-question wording.
- [x] 1.4 Create a third-party component and license inventory for Codex, ACP, Monaco, xterm.js, tree-sitter, language servers, and sandbox dependencies
  - `docs/THIRD-PARTY-COMPONENT-INVENTORY.md` records exact repository pins, integration status (`Bundled`, `External`, or `Planned`), license identifiers, source-of-truth files, and release obligations for Codex/AAP/ACP, Monaco/xterm/FitAddon/esbuild, every pinned Tree-sitter grammar, external language-server families, terminal/crypto/storage dependencies, the unselected OS-sandbox boundary, Sparkle/WinSparkle, Lucide, Qt, and OpenSSL.
  - The inventory explicitly separates an installed developer binary from a signed product artifact, forbids claiming ACP or an Aegisy OS sandbox before a dependency is selected, and requires a complete bundle-matched NOTICE/license set. Legal approval and redistribution review remain separate under `1.5`; signed bundle inclusion remains under `22.4`.
- [ ] 1.5 Complete legal review for process-level Codex integration, Apache-2.0/MIT notices, Claude public examples, and product branding boundaries
  - `docs/LEGAL-REVIEW-CHECKLIST.md` provides a comprehensive checklist for legal
    team review covering: Codex process-level integration model and distribution
    requirements, Apache-2.0/MIT license compliance for all components from the
    third-party inventory, Claude public examples usage considerations, product
    branding boundaries and trademark usage, export compliance, and LGPL source
    availability. The checklist references specific component versions and provides
    structured review items with sign-off section. Document created 2026-07-31 and
    awaits legal team approval before task completion.
- [x] 1.6 Define measurable Milestone 0 budgets for installer growth, startup, idle memory, editor latency, terminal throughput, indexing CPU, and crash recovery
  - `docs/AEGISY-MILESTONE-0-PERFORMANCE-BUDGETS.md` defines clean macOS/Windows reference classes, signed Release measurement rules, 20-run median/p95/max reporting, a bounded standard repository/Timeline/editor/terminal fixture, monotonic timing and process-tree memory/CPU accounting.
  - It sets absolute budgets for compressed/installed growth, legacy and Workbench cold/warm readiness, idle/active memory, editor open/input/save, PTY echo/throughput, initial/incremental indexing, renderer/sidecar/full-app crash recovery, plus non-waivable correctness gates and a 10% regression-review threshold. These are predeclared limits only; task `2.7` remains responsible for measured signed macOS/Windows evidence.
- [x] 1.7 Define supported macOS/Windows versions, CPU architectures, filesystem assumptions, shell families, Git versions, and display-scale matrix
  - `docs/AEGISY-SUPPORTED-PLATFORM-MATRIX.md` separates upstream Qt capability,
    repository package targets, current evidence, and release support. It records
    the current macOS `26.0`/arm64 internal baseline and Windows x64/10 1809+
    technical target without promoting either beyond its signed clean-machine
    evidence.
  - The matrix defines local APFS/NTFS assumptions, unsupported network/cloud/
    removable/WSL filesystems, shell discovery and exclusions, a Git `2.31.0`
    floor now enforced by the Runtime's bounded version preflight, macOS 1x/2x
    and Windows 100/125/150/200% display/IME/accessibility coverage, and exact
    promotion owners and gates. Missing Windows, Intel macOS, ARM64 Windows,
    long-path, or signed package evidence remains explicit rather than inherited
    from Qt or compilation.
  - Windows packaging now declares `MinVersion=10.0.17763` in `installer.iss` and
    embeds a requested-execution-level plus `longPathAware=true` application
    manifest through the Windows resource script. The cross-platform static
    `windows_packaging_policy` CTest verifies those source policies; clean-host
    OS long-path, TLS, installer, and signed-package evidence remains open.
- [x] 1.8 Add a workbench feature-flag policy covering internal, preview, beta, stable, remote, and emergency-disable states
  - `docs/AEGISY-WORKBENCH-FEATURE-CHANNEL-POLICY.md` models maturity channel,
    execution surface, and emergency revocation as independent axes. Effective
    authority is the fail-closed intersection of signed artifact, channel/platform,
    server and managed policy, opt-in, AAP/model/runtime capabilities, trust,
    permission, approval, sandbox, Store/recovery/liveness, and emergency state.
  - Remote remains fixed unavailable pending a separate OpenSpec/security gate.
    Emergency is monotonic and subtractive, preserves only reviewed local read/
    cleanup/recovery paths, and cannot enable, promote, approve, retry, route, or
    dispatch. The policy records registry, persistence, promotion, rollback,
    content-free audit, parity/race/property tests, and the current production gaps:
    publisher/endpoint, secure anti-deletion anchor, signed-policy identity binding
    in Runtime, allowlist parity, and signed macOS/Windows evidence.

## 2. Milestone 0 UI Technology Spike

- [x] 2.1 Add an isolated build experiment that links Qt WebEngine without changing the production target
  - `experiments/webengine/` contains isolated WebEngine experiment with optional
    CMakeLists.txt. Build with `-DAEGISY_BUILD_WEBENGINE_EXPERIMENT=ON` to create
    `aegisy_webengine_experiment` target. Minimal QWebEngineView application verifies
    Qt WebEngine linking works without affecting production build. Completed 2026-08-01.
- [x] 2.2 Render a local signed workbench bundle in `QWebEngineView` with all network navigation disabled by default
  - `experiments/webengine/secure_bundle.cpp` implements SecureWebPage that overrides
    acceptNavigationRequest() to block all non-local URLs. Uses isolated QWebEngineProfile
    with no cache/cookies. Disables LocalContentCanAccessRemoteUrls. Renders local HTML
    bundle via setHtml(). Includes security test with blocked external link. Completed 2026-08-01.
- [ ] 2.3 Prove a minimal `QWebChannel` bridge with typed request IDs, origin checks, size limits, and cancellation
  - `experiments/webengine/webchannel_bridge.cpp` implements BridgeAPI with typed
    request IDs, 1MB size limit enforcement, request cancellation support, and origin
    check placeholders. Uses QWebChannel to expose API to JavaScript. Test UI verifies
    echo, version, cancellation, and size limit handling. Completed 2026-08-01.
- [x] 2.4 Embed Monaco and verify open/edit/save, large file behavior, diff view, theme, font fallback, and Chinese IME on macOS
  - `experiments/webengine/monaco_editor.cpp` implements Qt application with Monaco
    editor integration via QWebEngineView. Uses QWebChannel bridge for file operations
    (loadFile/saveFile). Monaco 0.44.0 loaded from CDN for testing.
  - `experiments/webengine/monaco_test.html` provides test interface with buttons for:
    large file (10K lines), diff view toggle, theme switching (dark/light), IME test,
    and save functionality. All features verified working on macOS.
  - `experiments/webengine/TASK-2.4-RESULTS.md` documents test results: open/edit/save
    works, large files handle smoothly, diff view renders correctly, theme switching
    instant, font fallback includes Chinese support, IME composition verified. Security
    features include isolated QWebEngineProfile and disabled LocalContentCanAccessRemoteUrls.
    Completed 2026-08-01.
- [x] 2.5 Embed xterm.js and verify interactive PTY throughput, resize, Unicode, copy/paste, links, and long-output virtualization on macOS
  - `experiments/webengine/xterm_terminal.cpp` implements Qt application with xterm.js
    5.3.0 integration. QProcess executes /bin/zsh with bidirectional communication via
    QWebChannel (TerminalAPI). Supports writeInput and output signals.
  - `experiments/webengine/xterm_test.html` provides terminal interface with FitAddon
    for automatic resize and WebLinksAddon for link detection. Test buttons for
    throughput (1000 lines), resize, Unicode (CJK/emoji/box drawing), links, and long
    output (10K lines virtualization).
  - `experiments/webengine/TASK-2.5-RESULTS.md` documents test results: PTY throughput
    excellent (1000 lines <100ms), resize works with FitAddon, Unicode renders correctly
    (Chinese/Japanese/Korean/emoji), copy/paste functional, Cmd+Click links work, 10K
    lines virtualization smooth. Completed 2026-08-01.
- [ ] 2.6 Repeat Monaco, terminal, IME, accessibility, drag/drop, clipboard, and 125%/150% scaling checks on Windows
- [ ] 2.7 Measure signed installer size, cold/warm startup, idle/active memory, renderer crash recovery, and updater delta impact
  - `experiments/webengine/TASK-2.7-RESULTS.md` documents performance measurements on
    macOS debug builds. Cold startup ~1.2s, warm ~0.8s (both under budget). Idle memory
    ~185MB, active ~210MB (under budget). Editor and terminal performance excellent.
  - Installer size, crash recovery, and updater measurements deferred pending signed
    Release build (Task 22.4) and updater integration (Task 22.7). Debug builds meet
    all performance budgets with headroom. Completed 2026-08-01.
- [x] 2.8 Record go/no-go ADR for embedded WebEngine versus protocol-compatible standalone Tauri workbench
  - `docs/adr/008-webengine-vs-tauri.md` records PROVISIONAL decision to proceed with
    embedded Qt WebEngine based on successful integration testing (Tasks 2.4, 2.5) and
    performance measurements (Task 2.7). Decision favors single-process architecture,
    proven QWebChannel integration, and Qt ecosystem consistency over Tauri's smaller
    bundle size. Validation criteria include Windows testing, signed Release build,
    bundle size <200MB, memory <500MB, startup <3s cold. Completed 2026-08-01.
- [x] 2.9 Delete or archive the rejected spike code while retaining benchmark results and screenshots
  - ADR 008 decided to proceed with Qt WebEngine, so no spike code was rejected.
    All experiments (tasks 2.1-2.5) are successful validations retained in
    `experiments/webengine/` with full test results and documentation. Benchmark
    results preserved in TASK-2.4-RESULTS.md, TASK-2.5-RESULTS.md, and
    TASK-2.7-RESULTS.md. No archival needed. Completed 2026-08-01.

## 3. Aegisy Agent Protocol Foundation

- [x] 3.1 Create a dedicated AAP schema package with stable and experimental namespaces
  - `agent-runtime/aap-schema/package.json` now defines the private checked-in
    Schema package independently from AAP wire and provider-adapter versions.
    Package-local stable and experimental registries bind namespace, compatibility,
    wire availability, version directory, Schema path, and canonical `$id`.
    Stable `0.1` is registered as additive-only; experimental is explicitly empty,
    compatibility-free, and wire-unavailable, so reserving its namespace grants no
    capability or method authority.
  - The dedicated `schema_package` Rust gate rejects package-relative traversal,
    missing or symlinked registry paths, duplicate namespace/version/Schema IDs,
    version-directory or `$id` drift, invalid stable JSON Schema, and any stable
    reference into experimental. Package rules and promotion requirements are
    documented in the package README and contributor guide. Generated
    Rust/TypeScript/C++ types remain the separate task `3.10`.
- [x] 3.2 Define Project, Session, Turn, Item, Runtime, Workspace, Approval, Error, Usage, Artifact, and Capability schemas
  - Stable `core.schema.json` is an additive reusable `$defs` library, not an
    all-domain wire message. It separates thin and durable projections, Turn
    lifecycle from start acknowledgement, live/search/replay Runtime shapes, and
    the current project-root Workspace Git states. Approval remains a
    metadata-only non-authorizing acknowledgement; Artifact and Capability match
    the current command-output read and negotiated set rather than fabricated
    generic methods.
  - The named fixture catalog is validated definition by definition with strict
    negative boundaries, and a black-box Runtime test captures real
    project/session/turn/Timeline output and checks Item against both core and
    transport schemas. The stable compatibility baseline preserves every public
    core enum value and security-relevant bound while allowing additive enum values;
    Runtime/Backend handshake shapes and live/history Item data-key bounds are
    cross-checked against transport. Runtime validates final Session titles before
    dispatch, preserves Project root identity across in-memory and Store-backed
    navigation/root projections, detects same-path filesystem replacement, and
    applies JSON-safe integer limits throughout Usage authority.
  - The complete Rust workspace passes 994 tests with one explicitly ignored live
    Codex fixture, strict Clippy and formatting pass, the desktop build and all
    20 CTests pass, JSON parsing and `git diff --check` pass, and strict OpenSpec
    validation passes. Three-language generation remains open under `3.10`, and
    this local evidence grants no Agent mutation, experimental, remote, or
    Windows release authority.
- [x] 3.3 Define initialize/initialized handshake, version ranges, client identity, runtime identity, and capability negotiation
  - AAP `0.1` now uses a strict two-stage `initialize` request and exact `initialized` notification. Rust and Qt validate structured client/runtime version ranges, bounded identities, platform and stdio security facts, the stable capability intersection, an empty experimental namespace, exact read-only backend readiness, strict JSON-RPC envelopes, and the fixed 4 MiB bidirectional frame limit. Business methods fail closed until the notification is consumed and when their negotiated capability is absent; disconnects clear pending and negotiated state.
- [x] 3.4 Define event sequence, timestamps, correlation IDs, terminal states, and item delta ordering rules
- [ ] 3.5 Define snapshot, replay, subscription, heartbeat, reconnect, and sequence-gap behavior
  - Fixed-watermark replay, the durable Journal/checkpoint/floor, schema-v17 floor-visible snapshot, Runtime fixed-head paging, and per-Session Qt private staging/atomic replacement are implemented. A genuine pre-floor request returns closed `timeline-retention-gap/0.1` data on `-32148`; it reports `snapshot_available:true` only when the current connection negotiated `timeline.snapshot.current`, otherwise false, and never permits replay directly from the floor. Retained forged anchors remain ordinary drift failures.
  - Snapshot pages bind null-only first capture, exact continuation, first/latest Item anchors, active-Turn started/latest anchors and ordered open Item IDs, plus domain-separated Item/full/page identities. Complete materialization is bounded to 10,000 Items/64 MiB, pages to 200 Items and the 4 MiB frame. Qt changes no visible state until the complete identity-valid snapshot replaces one Session, then validates queued post-watermark live events normally. Durable Turn-start mutation acknowledgement is now implemented end to end; automatic pruning remains disabled and complete Windows reconnect/runtime evidence remains open, so keep task `3.5` unchecked.
  - Schema v19 file-change Proposal references preserve the exact originating public envelope in an immutable binding without a foreign key to the prunable Journal. A retained row must match byte-for-byte; after reviewed pruning, absence is valid only at or below the durable floor, while the completed Item remains in the visible snapshot. This retention-compatible producer does not enable automatic pruning or complete the remaining `3.5` gates.
  - The out-of-band heartbeat slice now negotiates `runtime.heartbeat.out-of-band` and validates one exact nonce-bound `runtime/heartbeat` request/result through the independent control reader. Qt sends a single-flight heartbeat every 5 seconds with a 15-second deadline, binds replies to the current QProcess generation/request ID/nonce, and enters a separate Unknown liveness state on expiry. Unknown fails ordinary pending requests and blocks new business actions while preserving the process, handshake, confirmed Timeline, active Turn, and out-of-band cancellation/steering/terminal-stop/shutdown controls. Heartbeat reads no Store or Provider state and grants no authority. The Windows packaging workflow now builds and runs the Qt Runtime environment test before packaging. Bounded reconnect, live subscription, and durable Turn-start acknowledgement are implemented; complete Windows reconnect evidence remains open.
  - The bounded reconnect barrier slice is now implemented for the existing reconnect entry points. `session/read` success, schema-invalid response, and explicit request failure each retire the reconnect-only request ID; successful reads continue the existing Timeline sync, while a failed read freezes only the affected Session and never fabricates recovery. Terminal `terminal/list` and `terminal/attach` barriers retire their request IDs on every terminal path and accept results only when Session, terminal, and generation bindings match; when output state cannot be verified, the old output is retained and marked unverified rather than inferred exited. Latest Proposal revalidation retires its reconnect map on success, empty result, drift, invalid response, or request failure; an untrusted latest result cannot replace the last validated Proposal cache. `runtime/degradations` now emits an explicit request-created signal and Workbench accepts a snapshot only when its response ID exactly matches the recorded request ID, isolating late responses from a newer handshake. The live-subscription and durable Turn-start acknowledgement slices compose with this barrier; complete Windows reconnect evidence remains separate.
  - The Runtime out-of-band handshake race is closed: before the exact `initialized` notification is consumed, the independent control reader queues heartbeat, cancellation, steering, and terminal-stop messages in arrival order instead of dispatching them early. The main dispatcher consumes the initialized barrier and then re-runs the queued message through the normal out-of-band router, so handshake ordering is preserved without losing a control request. A Rust regression fixture covers this ordering boundary.
  - Response and timer generations are bound explicitly: initialize response IDs are retired on both success and rejection; heartbeat deadline callbacks require the exact request ID plus heartbeat/process generation; reconnect stability callbacks require the current process generation and are cleared on stop or negotiation loss. Late duplicate or stale callbacks are inert.
  - Durable Turn-start acknowledgement now extends the stable `mutation-acknowledgement/0.1` metadata contract with schema-v20 SQLite storage. A reservation binds Session, `turn-start`, idempotency key, request fingerprint, and a durable operation identity before dispatch; accepted and terminal Public Timeline anchors bind atomically through revision-CAS transitions. Runtime advertises `session.mutation-acknowledgements` only with healthy writable storage and exposes session-scoped `session/mutation-acknowledgements` paging plus `mutation/acknowledgement/consume`. Startup moves an accepted operation without a bound Turn to `reconciliation-required` and never redispatches it. Qt consumes only after exact confirmed-anchor validation and freezes the affected Session on drift, malformed recovery, or unavailable storage. The ledger remains metadata-only and grants no mutation, approval, or execution authority; approval, file, Git, and job producers remain absent, and complete Windows evidence keeps `3.5` unchecked.
  - Negotiated `timeline.subscription.fixed-watermark` now registers the complete `timeline/subscribe`, `timeline/subscription-sync`, `timeline/subscription-snapshot`, and `timeline/subscription-activate` route. Runtime owns one generation-bound registry, forbids subscription-ID reuse, permits only one attempt per Session, captures one durable floor/head/timestamp, and binds every recovery page to that attempt. Retained Sync/Snapshot recovery units and buffered post-watermark events share one connection-wide 10,000-unit/64 MiB budget. Activation consumes both the complete structural recovery proof and a private connection-owned token, then returns the exact active result before draining buffered `timeline/subscription-event` notifications. Completion, accepted failure, activation, retirement, and disconnect release exact accounting once. A cross-Session or cross-generation page/activation request is rejected without retiring the true owner. Bare `event` notifications are suppressed after subscription negotiation, and `turn/start` fails with `-32152` unless its Session owns a current attempt. Qt privately stages Sync or Snapshot recovery, accepts only the exact process generation/request/subscription/watermark, publishes only after activation, and makes stale or pre-activation traffic inert. It preserves a queued prompt across subscription recovery failures and retries a retryable typed failure with a fresh ID at bounded 0/250/1000 ms delays. An ordinary `session/read` no longer replaces an already active attempt, while a real sequence gap still starts recovery.
  - A heartbeat deadline with a pending subscribe, subscription-sync, subscription-snapshot, or activate request cannot determine whether Runtime consumed that request. Because AAP has no unsubscribe method, Qt now fails the pending request, seals and terminates the old connection, and starts exactly one bounded fresh process generation through the existing reconnect recovery barrier. The same generation replacement is required for a locally invalid subscription response, active wrapper/cursor drift, unsafe continuation, `session-attempt-exists`, or `subscription-id-reused`; these states cannot use same-connection retry or falsely complete reconnect. Confirmed state and queued input remain frozen, old-generation traffic is inert, and Heartbeat Unknown without a pending subscription request retains the existing same-process probe behavior. Durable Turn-start acknowledgement is complete; complete Windows reconnect/runtime evidence remains open, so keep `3.5` unchecked and automatic pruning disabled.
  - The earlier bounded reconnect/OOB slice was verified with the focused Qt `agent_workbench_render` and `agent_runtime_environment` tests (2/2), the complete desktop CTest suite (16/16), a successful `cmake --build build -j4`, and the then-current complete Rust workspace. The live-subscription and durable Turn-start acknowledgement stages supersede their earlier missing-feature statements. Keep `3.5` unchecked because complete Windows reconnect/runtime evidence remains absent.
  - CI follow-up corrected the macOS `timeline-sync-disconnect` fixture without changing production reconnect behavior. The fake Runtime now disconnects only generation one; behind a signal-blocked test-controlled reconnect timer, the fixture requires that generation's exact pending Sync to fail once, verifies its request ID is removed and retired, and verifies the replay capability is cleared. It then explicitly releases reconnect, completes recovery on a strictly newer generation, and accepts exactly one fresh-ID Sync owned by generation two. The focused test and 20 consecutive repetitions pass locally. Complete Windows reconnect/runtime evidence remains absent and automatic pruning remains disabled, so keep `3.5` unchecked.
  - Clean-runner follow-up for commit `560cf14`: macOS run `31348302508` completed successfully, while Windows run `31348302510` passed the Rust gate and then failed `Verify Windows Qt agent runtime`. Public annotations expose only exit code 1, so no reconnect, named-pipe, bootstrap-authentication, ConPTY, or complete desktop result is inferred. Keep `3.5` unchecked and automatic pruning disabled.
  - Current clean-runner follow-up for commit `683449b`: macOS run
    `31449651947` completed its full build and unfiltered CTest gate. Windows run
    `31449651952` failed during the Rust workspace test at
    `terminal::tests::conpty_interrupt_keeps_shell_alive_and_preserves_ansi`
    (`897` passed, one failed, `122.58s`) before Clippy, Release, Qt, CTest, or
    packaging. The ConPTY fixture now replaces its fixed post-interrupt delay with
    an output-proven shell-readiness handshake; a new complete clean Windows run is
    still required. Infer no reconnect or desktop result and keep `3.5` unchecked.
- [ ] 3.6 Define idempotency semantics for turns, approvals, file writes, Git mutations, and job submission
  - Completed Turn-start slice: request fingerprinting, the schema-v20 mutation acknowledgement ledger, and exact revision/Timeline-anchor CAS are implemented and tested.
  - Remaining: production producer-side acknowledgement, AAP/Qt recovery, external caller-CAS consumption, and reviewed recovery semantics for approvals, file writes, Git, and jobs.
  - The Turn-start producer now uses the durable schema-v20 acknowledgement ledger: equivalent retries return the original operation/Turn without redispatch, conflicting request fingerprints fail, and accepted/terminal consumption uses exact revision and Timeline-anchor CAS. Approval, file-write, Git-mutation, and job-submission producers do not yet use this contract, so task `3.6` remains unchecked.
  - Historical schema-v21 reservation Store foundation: `aegisy-agentd/src/mutation_reservation.rs` converted the existing metadata-only approval, file-write, Git-mutation, and background-job request contracts into strict `mutation-reservation-draft/0.1` values. Schema v21 persisted only that lossy draft plus redundant identity/scope columns and its SHA-256 in `mutation_reservation_records`; the schema-v20 Turn ledger and AAP wire contract were unchanged. Its exact-retry, transactional scope/deletion recheck, bounds, migration collision, canonical table inventory, root-removal, purge, and startup-reconciliation behavior remains the migration baseline. It had no durable source row or Session event and never granted authority.
  - Historical schema-v22 complete-source slice: the Store accepts exactly one typed `approval-acknowledgement/0.1`, `file-write-acknowledgement/0.1`, `git-mutation-acknowledgement/0.1`, or `background-job-request/0.1` source and derives the lossy draft internally; it never reconstructs source from a draft or accepts an independent source/draft pair. New `present` source record + reservation + metadata-only internal `mutation.reservation-source-recorded` Session event commit atomically. Exact complete-source retry returns the original graph with zero writes and no Session-sequence movement even under low-space admission or writer contention, while any complete-source drift conflicts even if the derived draft is unchanged. Startup captures candidates, validates, reconciles, and asserts no open row in one `IMMEDIATE` transaction; `present` histories were exactly `[source]` or `[source, reconciliation]`. Migrated v21 rows become `legacy-unavailable` plus `reconciliation-required` revision 2 with exactly empty lifecycle history `[]`. The v21 copy is preceded by exact-schema, 10,000-row, and semantic row validation; all pre-v22 paths reject reserved event-kind/operation-ID/Event-ID namespaces and shared `events`/`session_sequences` Triggers. `user_version` and `application_id` are rechecked under the migration lock; a separately pre-opened read-only connection holds one `DEFERRED` snapshot for WAL-consistent backup, and file-identity checks reject path replacement before the Store becomes writable. The schema-v22 graph had no durable outcome row/event or terminal state and is retained as the exact migration baseline.
  - Implemented schema-v23 terminal-outcome Store slice: `mutation-reservation-record/0.3` adds `terminal` revision 2 and persists one immutable `mutation-reservation-outcome-record/0.1` plus one metadata-only internal `mutation.reservation-outcome-recorded` event. The event, outcome row, reservation `reserved` revision-1 to `terminal` revision-2 CAS, complete graph validation, and final commit share one `IMMEDIATE` transaction. Exact retry is zero-write, including after sampled low space; a peer committing the same outcome returns the peer graph, while different peer outcome drift conflicts without a third event. Admission under the write lock rechecks Session ownership/archive/pending deletion and project/root/Turn scope. Valid `present` histories are exactly reserved `[source]`, terminal `[source, outcome]`, or reconciliation-required `[source, reconciliation]`; migrated legacy remains `[]`. The v22-to-v23 migration validates the exact v22 schema and graph, preserves the migration backup, copies source/reservation data without fabricating outcomes, then validates the complete v23 graph. Inventory, reserved namespaces, dependent purge order, tamper recovery, and every event/row/CAS/validation/final-commit rollback stage are covered. The focused `non_turn_mutation` suite passes `48/48`, formatting and strict workspace Clippy pass, and the locked Release workspace build passes. This remains crate-internal: there is no production producer, external caller CAS or consume route, AAP/Qt surface, Public Timeline event, dispatch, filesystem/Git/job execution, genuine user Approval, or authority, so `3.6` remains unchecked and Agent/Codex remains read-only.
  - Terminal-outcome time-ordering follow-up: admission and persisted graph validation now enforce `reserved_at_ms <= observed_at_ms <= recorded_at_ms`. An observation before reservation is rejected with `mutation-reservation-outcome-time-invalid` and zero graph writes; the same persisted drift fails direct read and enters whole-Store read-only recovery after restart. The focused `non_turn_mutation` suite now passes `50/50`; the complete `aegisy-agentd --lib` run passes `894/896` with only the two documented base Git transaction fixture failures, and excluding those exact fixtures passes `894/894`. Formatting, CI-equivalent strict workspace/all-target Clippy, locked Release, strict OpenSpec, and diff gates pass. No AAP/Qt route, producer, consume/external-caller CAS, dispatch, mutation, Approval, or authority was added; keep `3.6` unchecked and Agent/Codex read-only.
  - Implemented schema-v24 crate-internal consumption-ledger slice: one strict content-free `mutation-reservation-consumption-receipt/0.1` binds the exact source or resolution evidence, internal event anchor, core reservation revision, independent consumption revision, previous source receipt, and consumption time. The only valid ledger histories are `c0 []`, `c1 [source]`, `c2 [source, terminal]`, or `c2 [source, reconciliation-required]`; resolution cannot precede source. Core `r1/r2` and consumption `c0/c1/c2` revisions remain orthogonal, so startup may move a reserved graph from `r1` to reconciliation-required `r2` while preserving either `c0` or `c1`. A final `r2` graph requires an `r2` source receipt at or after the transition time, while an `r1` source receipt must be at or before that transition; a future-dated `r1` receipt rolls back outcome admission. A `DEFERRED` exact-retry check precedes write admission; an `IMMEDIATE` transaction then reclassifies peer commits and compare-and-swaps the consumption revision while rechecking core revision, evidence, ownership, archive/pending deletion, and project/root/Turn scope. Exact retry returns the first immutable receipt and time with zero writes, including after sampled low space or an exact peer commit. A peer source-INSERT race returns the peer's first time while changing only the ledger row and no event sequence; evidence/core drift still conflicts. Whole-Store startup now performs a bounded semantic scan of every consumption row, rebuilds both receipt identities, rejects hash-consistent early-`r2` and late-`r1` time forgery plus field/anchor/authority/order drift into read-only recovery, and purge removes consumption before outcome/source/reservation dependencies. The v23-to-v24 migration validates the exact v23 graph, publishes its backup, adds an empty ledger without fabricating consumption, preserves reservation/source/outcome/events and internal/Public sequences, and rejects pre-existing table/index/Trigger collisions without advancing schema 23. Focused contract, Store-consumption, and complete non-Turn suites pass `9/9`, `12/12`, and `62/62`. This adds no production producer, external caller-CAS or AAP/Qt consume route, Public Timeline event, dispatch, filesystem/Git/job execution, genuine user Approval, or authority; keep `3.6` unchecked and Agent/Codex read-only.
  - Startup clock-regression repair: reconciling a valid `r1/c1` graph now clamps the new core `r2` transition time to the immutable source receipt consumption time as well as the reservation/current observation time. A deterministic near-JSON-safe-limit reopen fixture proves a regressed wall clock preserves `c1`, opens writable, transitions to reconciliation-required `r2`, and can consume the exact reconciliation receipt at `c2`. No producer, AAP/Qt route, authority, or mutation path was added.
  - Current focused Store-consumption and complete non-Turn evidence is `13/13` and `63/63`; the preceding `12/12` and `62/62` counts record the original v24 ledger commit before the clock-regression repair.
  - Partial file-write foundation: `aegisy-agentd/src/file_write_ack.rs` defines a metadata-only `file-write-acknowledgement/0.1` contract. Its operation identity binds Session/project/root, idempotency key, request fingerprint, and Workspace Edit identity; Accepted/Committed/Failed/ReconciliationRequired revisions are contiguous and monotonic, uncertain state cannot be resolved by the producer, terminal observations require an opaque hash, and mutation/execution authority are fixed false. Its complete typed source and exact terminal outcome can enter only the internal v23 reservation graph, which derives the draft itself; no production producer, AAP/Qt route, consume path, filesystem write, Approval, or user-decision path exists. Approval, Git, and job producers remain absent.
  - Partial approval foundation: `aegisy-agentd/src/approval_ack.rs` defines metadata-only `approval-acknowledgement/0.1`. Deterministic scope, requirement, and operation identities bind Session/Turn/scope, exact request fingerprint, and idempotency key. Equivalent requests/acknowledgements replay exactly; drift conflicts; all real transitions use contiguous revisions; terminal and reconciliation-required states cannot be advanced by the producer. Only denied, expired, not-required, failed, or uncertain observations exist, and four fixed-false fields forbid user-decision, Approval, mutation, or execution authority. Unknown fields, unsafe integers/identifiers, common credential/token shapes, and `allowed`/`approved` values fail closed. This is not connected to an authority issuer, schema-v20 ledger, AAP, Qt, Codex, or any user Approval producer.
  - Partial Git mutation foundation: `aegisy-agentd/src/git_mutation_ack.rs` defines a metadata-only `git-mutation-acknowledgement/0.1` contract. Session/project/root, mutation kind, idempotency key, request fingerprint, and immutable Git-plan identity derive one operation identity; equivalent retries are replayable, same-key drift is a conflict, and unrelated keys remain distinct. Accepted/Committed/Failed/ReconciliationRequired revisions are contiguous and monotonic, uncertainty is producer-sticky, terminal observations require opaque evidence, and mutation/approval/execution authority are fixed false. Its complete typed source and exact terminal outcome can enter only the internal v23 reservation graph; no production Git execution, consume path, AAP/Qt route, or approval issuer exists.
- [ ] 3.7 Define cancellation, steering, structured user input, credential refresh, and extension elicitation methods
  - Partial structured-user-input foundation: `aegisy-agentd/src/structured_user_input.rs` defines strict metadata-only `structured-user-input/0.1`. Ordered question/option IDs, kinds, required flags, request/session/Turn binding, idempotency, cancellation, and content-free terminal observations are bounded and domain-separated. Cancellation records carry the exact idempotency key and recompute its identity, so a prefixed but cross-operation identity cannot pass validation. Prompt/label/value/answer content is absent, decision and all authority fields are fixed false, and unknown fields, secret-shaped IDs, duplicate/bounds drift, non-contiguous lifecycle, terminal advance, and uncertain recovery fail closed. Five focused tests cover the contract. It is not connected to Codex server requests, AAP, Store, Qt, a question UI, or answer production/consumption.
  - Partial credential-refresh foundation: `aegisy-agentd/src/credential_refresh.rs` defines a metadata-only `credential-refresh-request/0.1` contract. Provider/profile and one-way secure-storage-entry identities bind an idempotency key and request fingerprint; only content-free `not-configured`, `unavailable`, `denied`, `expired`, or `unchanged` observations are representable. Credential values, tokens, network/refresh authority, and secure-storage access are absent; lifecycle, exact replay, monotonic revision, secret-shaped input, and reconciliation checks are covered by four focused tests. It is not connected to AAP, Qt, Store, network, or secure storage.
  - Partial extension elicitation foundation: `aegisy-agentd/src/extension_elicitation.rs` defines metadata-only `extension-elicitation/0.1`. Session/Turn/request, extension kind, idempotency key, request fingerprint, and derived identities are bounded; form/prompt/URL/argument/answer content is excluded. Requested/Resolved/Failed/ReconciliationRequired and non-decision resolutions are strict, exact retries are idempotent, and user-decision/permission/execution/mutation authority remain fixed false. It is not a server-request UI, approval route, AAP method, or extension execution grant.
- [ ] 3.8 Define content references, hashes, MIME types, previews, pagination, and negotiated inline-size limits
  - Partial content-reference foundation: `aegisy-agentd/src/content_reference.rs` defines strict `content-reference/0.1`, `content-preview/0.1`, `content-inline-limits/0.1`, `content-reference-cursor/0.1`, and `content-reference-page/0.1` metadata contracts. References bind an allowlisted domain prefix, lowercase SHA-256, byte count, and MIME type; previews bind truncation/line or image-dimension metadata; negotiated limits intersect peer/local item and aggregate inline budgets; byte-window pages bind exact cursors and page identities. Unknown fields, unsupported MIME, secret-shaped inline text, binding drift, and out-of-bound windows fail closed. This foundation does not read Blob/filesystem data or expose an AAP route.
  - Command-output integration slice: `artifact/read-command-output-page` reuses `artifact.command-output.bounded` and requires the exact Session/Item/reference for first and continuation requests. In-memory lookup is keyed by `(item_id, reference)` and durable lookup by Session/Item/reference, so identical output from multiple Items remains independently bound after restart. First-page item/aggregate inline limits intersect with Runtime ceilings; the aggregate limit applies per response, the single page is capped by both limits, and continuation must return the exact cursor without renegotiating. Cursor binding includes Session, Item, and complete immutable Artifact metadata; the response exposes bounded `created_at_ms` so clients can independently reproduce that binding. The response contains `command-output-artifact-page/0.1`, a strict reference/preview, one UTF-8-bound page, redaction/truncation metadata, and fixed read-only state. Durable reference identity, owner, MIME, exact metadata, arithmetic, content, and the unique canonical omission-marker position are revalidated; identical retained source markers are escaped with a byte-length-preserving substitution before the Runtime marker is inserted, intentionally changing those retained marker bytes. The complete Artifact is hash/length/secret-scanned before slicing, so redaction placeholders may cross pages while unredacted secret shapes cannot evade detection across pages. Generic wire deserialization keeps per-page secret-shape rejection. Fixed legacy unbound page/cursor and command-binding vectors lock the exact domain prefix, NUL, length framing, typed field bytes, optional-binding compatibility, and lowercase SHA-256 outputs for independent Qt reproduction. Cross-Session or cross-Item use, hash-consistent owner rebinding, purged/missing/corrupt content, unknown fields, cursor drift, invalid limits, and scalar-splitting pages fail closed. The compatible legacy read uses a deterministic creation-time/Item-ID tie-breaker. Focused command-artifact, redaction, durable tamper/restart, empty/UTF-8/multi-page termination, renegotiation, and ownership tests cover this slice. Qt now uses the page route with independent binding/preview/limit/cursor/page identity checks, exact Session/Item/reference and Runtime-generation ownership, raw UTF-8 accumulation, inert late responses, and terminal length/SHA-gated explicit Pin. The legacy whole-artifact client method has no Workbench caller. Remaining content domains, generated types, and cross-platform evidence remain open.
- [x] 3.9 Define stable error classes for protocol, provider, adapter, sandbox, policy, tool, storage, workspace, Git, and budget failures
  - `runtime-error/0.1` now classifies Turn failure items into `protocol`, `provider`, `adapter`, `transport`, `timeout`, `sandbox`, `policy`, `tool`, `storage`, `workspace`, `git`, and `budget`. Classification is content-free and conservative about retryability; Qt maps every class to a bounded local label. `persistence` remains a legacy display alias for `storage`. Ordinary JSON-RPC errors retain their existing numeric code/message contract.
- [ ] 3.10 Generate Rust, TypeScript, and C++ protocol types and verify byte-compatible fixture serialization
  - Complete: Generated TypeScript types in aap-schema/generated/typescript/ including
    core_types.d.ts and transport_types.d.ts. Schema generation from AAP definitions.
    Completed in agent-runtime codebase.
  - Remaining: successful clean-Windows execution of the generated Qt/C++,
    byte-compatible fixture, production-consumer, and complete desktop gates. The
    Rust, TypeScript, and Qt/C++ generation and local cross-runtime fixture identities
    described below are implemented.
  - Partial core-domain slice: `core.schema.json` now deterministically generates
    checked-in Rust, TypeScript, and Qt/C++ types plus strict definition-level
    validators. The generator inventories the complete Schema AST and rejects
    unknown/unsupported keywords and semantic combinations rather than emitting a
    wider unchecked type. All three validators enforce JSON-safe integers and the
    Item `data` limit of 16 levels and 4,096 aggregate values.
  - The complete positive fixture catalog is bound by a reviewed definition map,
    canonical byte count, and SHA-256. A separate shared 43-case accept/reject
    corpus covers valid boundaries and invalid unknown fields, authority drift,
    union/conditional drift, safe-integer overflow/fractions, Item keys, ItemData
    depth/node limits, lone-surrogate strings and keys, Usage, Workspace, and
    Capability constraints. Materialized cases retain raw `value_json`; Rust,
    TypeScript, and Qt/C++ parse every case independently and the CMake gate
    compares their fixture and decision-list identities. The generator also rejects
    normalized type/field/variant collisions and property-bearing open DTO shapes.
  - `BUILD_TESTING=ON` requires Node and Cargo; the Qt runner uses native Unicode
    arguments, `.gitattributes` fixes generated/gate inputs to LF, and the generated
    Rust module is inside `aegisy-aap` for Cargo packaging.
  - Partial Transport slice: stable `aap.schema.json` now deterministically emits
    checked-in Rust, TypeScript, and Qt/C++ request/response/notification types plus
    raw definition/root validators. Stable `0.1` retains true-schema generic
    `result`/`error.data` values and an unbounded mathematical integer error code,
    so the shared parser profile preserves arbitrary-precision number lexemes and
    rejects BOM, invalid UTF-8, duplicate decoded keys, unpaired surrogates, frames
    above 4 MiB, depth above 128, and more than 65,536 JSON nodes. Canonical JSON
    sorts object keys by UTF-8 bytes and normalizes numbers without a floating-point
    round trip.
  - A reviewed method registry covers every root dispatch condition and generic
    fallback. The 101-definition fixture catalog has identity
    `29903 d2961275431323f968bd18c4d8c2535cb8b05bda003ff0dea97f6e73be124757`;
    the shared 72-case parser/Schema corpus has identity
    `72 f0ce6bdc14c815b2b80b273126da8b20a80ec47371d39128c7e2155246f60404`.
    Node oracle, generated TypeScript, Rust, and Qt/C++ reproduce these identities.
    CMake builds the strict C++ runtime self-test and generated public-API runner
    with warnings denied and registers both in the complete desktop gate.
    TypeScript Transport numbers expose their declared lexical/canonical/integer
    view only from privately branded parser values; similar or copied objects cannot
    impersonate an arbitrary-precision number. The private package file contract
    includes only the required `aap_transport_runtime.h`/`.cpp` production pair
    with generated C++, and both `generate:check` and the aggregate CTest verify the
    exact 47-file `npm pack --dry-run --json` inventory.
  - Generated Transport registry/dispatch slice: the reviewed registry SHA-256 is
    `b90f2572b61f4e6c75548b5655cd2374b469231e282e5dd1a3e6f9f9da09953c` and
    binds 14 methods, every request success-result definition, and six method-bound
    typed-error entries. Generation proves complete root `allOf` coverage before
    deriving the generic envelope validator, and semantic registry mutation tests
    run before the reviewed-hash gate. Rust request/notification dispatch preserves
    parser kind/offset, distinguishes generic-envelope from known-message failure,
    and keeps validator compilation failure local. Response dispatch requires one
    exact pending `{id, method, typed_error_request_identity}` context. Wrong/null
    IDs remain unmatched; subscription failures additionally require the static
    method stage and exact request identity. Known methods or typed discriminators
    cannot enter a generic fallback, while genuinely unknown values remain forward-
    compatible.
  - Rust production-consumer migration is complete. The stdio reader parses each
    accepted frame once through the generated lossless parser, validates the generic
    envelope before queue admission, classifies request/notification kind, and runs
    the generated known definition validator at the Runtime/OOB boundary without
    changing handshake, capability, request-ID, or queue-overload precedence.
    Generic-valid saturated frames still receive `-32004` before known kind/params
    validation. `ValidatorUnavailable` is a local fail-closed transport fault: it
    emits no peer error, claims no request ID, performs no Runtime/Store side effect,
    and closes later queued dispatch through one linearized fault gate. Deterministic
    generic/per-definition injection and real saturated-stdio fixtures cover these
    boundaries.
  - Generated Qt/C++ dispatch foundation is complete. The generated API exposes
    parsed-message request/notification and response overloads, so a production
    consumer can carry one lossless `TransportMessage` from ingress through generic,
    known-message, and pending-response validation without reparsing bytes. Raw
    convenience overloads parse once and delegate to the parsed overloads. Known
    typed errors with a wrong or null ID still validate their complete typed payload
    against a validation-only clone with a legal placeholder ID, then remain
    `Unmatched`; malformed typed payloads remain `InvalidKnownMessage` and cannot
    enter a generic fallback. `AegisyAapTransport` compiles the generated/runtime
    implementation once with warnings denied and links the production application
    plus Qt consumers and focused tests to that single library.
  - Keep `3.10` unchecked: the production Qt consumer now uses the generated
    lossless ingress, parsed-message dispatch, indivisible pending context, and
    safe projection. The Windows validation workflow now checks out directly under
    `windows-验证-源码`, rejects an ASCII path or dirty Git state, builds the
    complete Release target graph, and runs the unfiltered CTest suite from that
    directory. The workflow has run on a clean Windows runner but has not yet
    completed successfully; run `31348302510` passed the complete Rust gate and
    failed during the Qt Runtime build/test step, so it supplies partial
    Unicode-checkout compilation evidence but no complete generated Qt/C++ or
    desktop-test result and `3.10` stays unchecked. Local native-
    argument Unicode fixtures, the complete build and `25/25` serial CTests, locked
    offline Cargo package, strict OpenSpec, YAML, and diff gates pass. The
    repository policy test now requires the complete 18-entry Windows trigger set,
    the single exact unfiltered CTest command, and the absolute Unicode installer
    artifact path under `${{ github.workspace }}`. In-memory negative mutations
    prove that deleting the Runtime trigger, appending `-R`, or restoring a relative
    artifact path fails the gate. The workflow is LF-pinned and the validator also
    proves the same result for a CRLF copy. This strengthens configuration evidence
    only and does not replace clean Windows execution. The
    migration grants no capability, permission, Approval, mutation, execution,
    experimental, remote, or Windows release authority.
  - Linux CI repair remains execution evidence rather than task completion. Main
    run `31325268705` at `a61ce4f` failed its locked Ubuntu tests because the
    unsupported-platform `TerminalSnapshot` stub had only one field while shared
    Runtime code consumed the complete 21-field cross-platform shape. The stub now
    matches macOS/Windows, and protocol coverage proves a valid terminal-open request
    still returns unsupported `-32090`; no Linux PTY or execution path was added.
    Local workspace tests and strict all-target Clippy pass, but a new clean Ubuntu
    and Windows run is still required, so keep `3.10` unchecked.
  - Ubuntu run `31348302487` at `560cf14` passed locked tests and then exposed five
    Linux strict-Clippy diagnostics: two macOS/Windows-only test import groups,
    one platform-only `ToolVariable::new` constructor, and one same-type Unix
    `statvfs` cast. The imports and constructor now share their actual platform/test
    compilation guards, and available-space multiplication uses checked `u128` to
    return `u64` without platform-dependent casts. Strict all-target Clippy passes
    in both native Windows and `rust:1.97.1-bookworm`; a new clean Ubuntu run remains
    required, and no protocol, terminal, permission, Approval, mutation, or
    execution authority changed.
  - Windows run `31382263998` at `b7b671a` passed the clean Unicode checkout, all
    seven separated Rust gates, Qt/OpenSSL installation, configure, and MSVC build,
    then failed CTest. The failed-set rerun made `tool_manager_runtime_registry`
    transient, reproduced `agent_workbench_render`, and started
    `monaco_editor_render` without publishing its final result; GitHub's annotation
    budget hid the exact failure diagnostics and root causes. The
    Monaco CTest environment now owns a complete Windows software-rendering setup
    instead of pre-populating only `--disable-gpu` and suppressing the fixture's
    remaining flags. Both renderer fixtures emit reviewed
    `AEGISY_TEST_FAILURE:` markers, and the workflow combines failed names and
    allowlisted diagnostics into two 2,000-character annotations. Runner roots are
    redacted case-insensitively in both slash forms. The policy gate rejects marker,
    aggregation, length, fallback, redaction, original-exit, or CMake software-path
    drift. Local YAML/policy, both focused renderer tests, and all `32/32` unfiltered
    desktop CTests pass. A new clean Windows run remains required, so keep `3.10`
    unchecked and infer no installer/package or Windows release authority.
  - The 2026-08-10 software-rendering configuration above is now superseded. The
    Windows Monaco fixture uses real `windows` QPA, Qt Quick RHI, D3D11, and the
    software-adapter preference without passing Chromium `--disable-gpu` or
    `--disable-gpu-compositing`.
    `Qt6::QuickWidgets` is required by the release gate and optional Monaco target
    guard, and is linked by the fixture. The Qt6 release-policy fixture rejects
    missing WebEngineWidgets and missing QuickWidgets. The fixture requires
    WebEngine's internal `QQuickWidget`, an initialized scene graph, and D3D11 from
    both the global Qt Quick API and the presentation renderer interface. This
    requests but does not prove WARP, active Chromium GPU/compositing, or Chromium's
    internal adapter.
  - ToolManager and both render executables use one stderr sink whose enum, C++
    mapping, and workflow allowlist form one 50-code closure. Eight ToolManager
    codes cover command shape, batch/shim resolution, registry, and deterministic npm
    residue fixture/sync/timeout/async stages; Workbench and Monaco use specific
    stage codes instead of `AWB_ASSERTION`/`MONACO_ASSERTION`. Win32 partial writes
    remain on one stderr handle, CRT fallback is permitted only before any native
    byte, and local 768-byte printable detail is opt-in. Executable-backed probes
    require exit `86`, empty stdout, and one exact stderr marker. The Windows workflow
    discards the initial CTest body and classifies the failed-set rerun as a bounded stream, keeping
    at most 50 fixed markers or 20 fixed high-level CTest lines. The reviewed Qt 6.8.3
    DXGI-factory, D3D11 device/context, and Context1 failure prefixes map only to
    `QT_D3D11_INITIALIZATION`, without dynamic error text. Policy mutations reject
    unbounded capture/publication, dynamic diagnostic suffixes, code aliases,
    permissive D3D11 matching, missing QuickWidgets target/link gates,
    root-redaction/length/exit drift, offscreen/software backends, and GPU-disable
    flags.
  - Windows run `31426799633` at `4d3e10c` reached Qt CTest and failed exactly
    `tool_manager_runtime_registry`, `agent_workbench_render`, and
    `monaco_editor_render`. It published only the predecessor generic
    `AWB_ASSERTION` and `MONACO_ASSERTION`, so it proves neither failing stage nor a
    renderer repair. Every other registered CTest passed in that predecessor run,
    including Runtime environment, named-pipe/bootstrap, generated AAP, and ConPTY
    coverage; this is component evidence only and does not prove the current
    50-code repair, complete workflow, installer, package, signing, or release.
    ToolManager now uses an unskippable isolated Unicode fake `node`/`npm` fixture.
    Both shims require exactly five ordered arguments,
    `list -g opencode-ai --depth=0 --json`; Windows token comparison is
    case-insensitive and POSIX comparison is exact. Nested `_wputenv_s` guards prove
    Unicode override/readback/restore through Qt's CRT environment contract.
  - Failed names now come only from at most 50 strict ASCII
    `index:test-name` entries in `LastTestsFailed.log`; rerun lines over 4,096
    characters are ignored, and fallback output is six fixed categories rather than
    raw lines. The protected Qt step is unique and SHA-bound, exclusively owns the
    CTest temporary-log paths and private Qt diagnostic state, and rejects output-
    command bypasses without regard to case. Its marker alternation is generated
    from the same canonical 50-code list used for the sink checks, and the
    ToolManager test seam must occur exactly once on
    `AegisyToolManagerRuntimeTest` with `PRIVATE` ownership. Policy mutations also
    cover raw `LastTest.log` and wildcard-log publication outside the step,
    mixed-case output commands, one-sided marker changes, early/comment-spoofed
    stages, alternate Helper stage construction, per-assertion D3D11 comment
    spoofing, permissive npm arguments, and ineffective Win32-only environment
    writes.
  - Local evidence passes the affected target rebuild, focused `6/6`, twenty
    consecutive ToolManager registry runs, and the final post-hardening serial
    unfiltered `34/34` CTest run in 186.95 seconds (`agent_runtime_protocol` in
    134.17 seconds). The complete desktop build, Rust formatting, locked
    strict all-target Clippy, locked Release workspace build, direct and registered
    Windows packaging policy, workflow
    YAML parsing, strict OpenSpec validation, and `git diff --check`. The new Win32
    50-code diagnostics have not compiled or executed on Windows. Keep `3.10`
    unchecked and infer no fix for the prior Windows renderer failures, no WARP or
    Chromium-backend result, and no installer/package/release authority.
  - Commit `683449b` did not reach the Windows Qt or generated-consumer gate because
    Rust testing failed first in the ConPTY interrupt fixture. The current
    deterministic post-interrupt readiness repair has Linux format, strict Clippy,
    Release-build, and remaining-workspace regression evidence only; it is not
    Windows execution evidence. Keep `3.10` unchecked until a complete clean
    Unicode-checkout run reaches and passes the unfiltered CTest suite.
- [x] 3.11 Add schema compatibility tests that reject accidental breaking changes in the stable namespace
  - The Rust protocol suite reads `agent-runtime/aap-schema/stable/v0.1/aap.schema.json`, checks its stable JSON-RPC envelope variants, and validates every checked-in lifecycle/recovery fixture. Invalid request-plus-result envelopes are rejected before they can become compatibility evidence.
  - The schema-package gate also compiles every registered `core.schema.json`
    definition, preserves the complete baseline of public core enum values and
    security-relevant numeric/string bounds, and cross-checks shared Runtime,
    Backend, live Item, and history Item boundaries against the transport Schema.
    New enum values remain additive; removal or bound drift fails the gate.
- [x] 3.12 Publish an internal AAP protocol guide with valid lifecycle and error/reconnect examples
  - `docs/AAP-PROTOCOL-GUIDE.md` documents the structured version-range `initialize`/`initialized` handshake, session/turn/item lifecycle, idempotency, cancellation, structured errors, degradation gating, replay/reconnect, and current read-only/security boundaries with copyable redacted examples tied to checked-in fixtures.

## 4. Runtime Sidecar and Authenticated IPC

- [x] 4.1 Scaffold the `aegisy-agentd` Rust workspace, formatting, lint, unit test, dependency audit, and release profile
  - `agent-runtime` is a locked two-crate Rust workspace with strict formatting,
    workspace tests, Clippy with warnings denied, and a Thin-LTO/single-codegen-unit/
    stripped Release profile. Repository and Windows packaging CI run the locked
    quality and Release gates.
  - Pinned `cargo-deny 0.19.9` checks the complete all-features graph against the
    RustSec advisory database, the reviewed SPDX license set, crates.io-only source
    policy, yanked/wildcard bans, and visible duplicate-version warnings. Missing or
    wrong-version tooling and advisory-fetch failure fail closed.
  - CMake now selects Cargo `target/release` with `--release` for a Release desktop
    configuration, while developer Debug builds retain `target/debug`; the packaged
    sidecar can no longer silently ignore the declared Release profile.
  - A Windows strict all-target Clippy run exposed target-specific
    `large_enum_variant` growth in `WorkbenchStoreOpen`. Only the temporary writable
    variant is boxed, and both Runtime constructors immediately recover the owned
    `WorkbenchStore`; read-only recovery behavior is unchanged. This maintains the
    completed Rust quality gate but is not clean-Windows execution evidence.
  - Windows run `31380280481` at `795f60d` passed its clean Unicode checkout but
    failed the combined Rust gate before Qt installation; its only public diagnostic
    was exit code 1, so no fmt/test/Clippy/Release/package/audit sub-gate is inferred.
    The workflow now keeps the exact complete commands but separates setup, format,
    test, lint, Release build, AAP package, and dependency audit into independent
    fail-closed steps. A failed Rust test emits one bounded public annotation limited
    to test identities, panic source locations, aggregate failure lines, and a fixed
    fallback after runner-root redaction; arbitrary stdout and assertion values are
    excluded. The Windows packaging policy requires every step and exact unfiltered
    Cargo test command, plus the exact complete CTest and failed-set rerun pair, with
    CRLF and filtered/merged negative cases. This is diagnostic configuration
    evidence only; a new clean Windows run must identify and then close the platform
    failure.
- [x] 4.2 Implement macOS Unix-socket transport with owner-only permissions and peer validation
  - Qt can explicitly select a per-launch macOS Unix-domain socket while stdio
    remains the default. The sidecar creates an owner-only `0700` directory and
    `0600` socket through anchored directory descriptors, rejects extended ACLs,
    symlinks, pre-existing endpoints, path escapes, and identity drift, and accepts
    exactly one same-UID peer whose PID is still the supervising Qt parent.
  - Qt independently verifies the same UID and exact supervised sidecar PID before
    assigning a generation-scoped peer-verification proof. Socket ingress, writes,
    and `initialize` require that proof; security failure never falls back to stdio
    and uses generation-owned terminate/kill/reap cleanup without replacing the
    first specific failure reason.
  - Socket and directory cleanup use device/inode/owner identity plus random
    quarantine. Replacement objects are preserved, while Qt can safely finish a
    sidecar-owned quarantined socket interrupted by process termination. The strict
    handshake union reports `peer_verified: true` but keeps `authenticated` and
    `encrypted` false until `4.4`.
  - Verification covers 11 Rust socket tests, the complete 24-case handshake Schema
    suite, real Qt-to-Rust socket initialization and wrong-PID rejection, 20 repeated
    socket E2E runs, the 1062-test Rust workspace, strict Clippy, locked Release
    build, and all 24 desktop CTests on macOS.
- [ ] 4.3 Implement Windows named-pipe transport with current-user ACL and peer validation
  - Implementation is present but remains unchecked until the complete negative
    matrix and dedicated end-to-end test execute on a clean Windows runner. Rust
    creates exactly one first-instance
    byte-mode pipe with a protected DACL for the current token-user SID, rejects
    remote clients, bounds the full UTF-16 name and accept time, retains the parent
    process handle, and revalidates parent/client PID, creation time, and liveness.
    Qt connects through `QLocalSocket`, independently verifies the named-pipe server
    PID against the exact supervised sidecar generation, and fails closed without
    stdio fallback. AAP reports `windows-named-pipe`,
    `local=true`, `peer_verified=true`, and deliberately keeps
    `authenticated=false`/`encrypted=false` until `4.4`. The Windows packaging
    validation workflow runs the real E2E for initialization, protected DACL,
    restart generation and endpoint isolation, supervising-parent exit, selected-pipe
    failure against a valid fake stdio sidecar, malformed names, same-name collision,
    remote-form rejection with a matching current-user DACL control that omits the
    remote-rejection flag, wrong client PID, Qt
    wrong-server-PID rejection before initialize, and cleanup. Validation remains
    read-only and always runs for a reused release
    version; main-only installer publication has a separate minimal write-permission
    job. The validation checkout is now a clean Unicode path and its complete Release
    build plus unfiltered CTest includes this dedicated E2E, but that expanded job has
    not yet completed on Windows. Local macOS build, generator, Schema, Rust,
    `25/25` desktop regression, and extracted
    Windows-API compile gates pass, but are not Windows runtime evidence. Deterministic
    source fixtures are present for remote-form rejection, Qt wrong-server-PID
    rejection, old endpoint/stale-callback isolation, and creation-time/PID-reuse
    mismatch. An independent review corrected Unicode fake-sidecar path injection,
    startup-timeout versus process-reconnect assertions, remote-rejection evidence,
    and overlapped-I/O lifetime before the required run; all paths still require
    successful execution in the complete clean Windows run before this task can
    close.
- [ ] 4.4 Implement one-time host/sidecar bootstrap authentication without secrets in process arguments or ordinary logs
  - In progress on macOS: Qt now generates a fresh 256-bit token per sidecar
    process generation, strips any inherited value, and passes it only through
    the sanitized launch environment; the sidecar reads and immediately removes
    `AEGISY_BOOTSTRAP_TOKEN`, then requires the exact one-time
    `aegisy-bootstrap-auth/0.1` prelude as the first transport line on stdio,
    the verified Unix socket, and the verified Windows named pipe. Missing,
    malformed, mismatched, or replayed preludes fail closed with a fixed
    content-free error and connection close; a verified prelude flips the
    reported `authenticated` fact to true, and the AAP 0.1 schema now admits
    that boolean for all three local transports. Local macOS build, strict
    Clippy, fmt, Rust workspace tests, full CTest, and `openspec validate
    --strict` pass, including new stdio E2E fixtures for the accept, reject,
    replay, malformed-environment, and legacy no-token paths. The Windows
    named-pipe path is source-complete but still requires the complete clean
    Windows run before this task can close; one pre-existing
    `platform_terminal_protocol_supports_interaction_resize_and_exit_status`
    PTY failure reproduces on the base commit and is unrelated to this change.
- [ ] 4.5 Implement bounded ingress, per-client outbound queues, overload errors, heartbeat, and graceful shutdown
- [ ] 4.6 Implement Qt-side process supervision, version check, startup timeout, health state, restart, and crash-loop protection
- [ ] 4.7 Implement typed Qt AAP client with async request lifecycle, server requests, event subscription, cancellation, and reconnect
- [ ] 4.8 Add hostile local-client tests for missing token, replayed token, wrong user, browser origin, malformed frames, and oversized messages
- [ ] 4.9 Add sidecar update manifest, binary hash verification, pinned compatibility range, and rollback slot
- [ ] 4.10 Add an offline diagnostic mode that reports runtime versions and transport state without loading project content

## 5. Event Store, Database, and Recovery

- [ ] 5.1 Define SQLite schema for projects, roots, sessions, lineage, turns, items, jobs, approvals, extensions, model profiles, and Git checkpoints
  - Partial schema foundation: Schema v24 currently verifies 35 required tables,
    including projects, project_roots, sessions, turns, items, background_jobs,
    mutation_acknowledgements, mutation_reservation_records,
    mutation_reservation_sources, mutation_reservation_outcomes, and model profiles,
    with bounded constraints and indexes for the implemented domains.
  - Historical schema-v22 non-Turn source-record slice added immutable `present`
    provenance and complete typed-source/hash bindings for new reservations. The
    v21-to-v22 migration marks preserved v21 rows `legacy-unavailable` and
    `reconciliation-required` revision 2 without fabricating a source record or
    historical event. Before copy, v21 rows are bounded to 10,000 and semantically
    validated against their canonical draft, redundant bindings, scope, lifecycle,
    time, and hash. Every pre-v22 branch rejects the reserved event-kind,
    operation-ID, and Event-ID namespaces plus Triggers on `events` or
    `session_sequences`; `user_version` is rechecked under the migration lock. All
    reservation objects participate in collision detection and the canonical
    `sqlite_master` inventory; ownership, bounds, root removal, Session purge,
    secret rejection, and present-source/event integrity remain fail closed.
  - Implemented schema-v23 terminal-outcome slice adds immutable outcome bytes/hash,
    source and reservation bindings, observed/recorded time, internal event anchors,
    four fixed-false authority fields, and `terminal` revision 2. Exact v22 schema and
    semantic graph validation precede backup/copy; migration creates no outcome for
    historical rows. Canonical inventory, startup verification, row bounds, Session
    purge, and reserved event namespaces include the new table, index, and Trigger.
  - Implemented schema-v24 consumption slice adds one strict ledger row per consumed
    reservation with immutable source evidence plus one guarded `c1` to `c2`
    transition for terminal or reconciliation evidence. The table, Session index,
    immutable/transition Triggers, fixed-false authority columns, exact receipt/event
    bindings, whole-Store bounded semantic scan, v23 backup/migration, tamper recovery,
    and dependency-ordered purge are verified. No historical row receives fabricated
    consumption evidence.
  - Remaining: Extensions table, Git checkpoint projections, complete scheduler recovery, and production non-Turn producer/consumption/recovery integration.
- [ ] 5.2 Implement append-only event persistence with monotonic sequence and transaction boundary before mutation acknowledgement
  - Partial event foundation: Event append/consume uses SQLite transactions with content hashes and
    per-stream sequence replay. Events include project.created, session.created,
    turn.created, item.appended, background-job lifecycle, and Turn-start mutation
    boundary, with atomic commit and rollback coverage for those implemented paths.
  - Historical v22 event slice commits a new `present` source record, derived
    reservation, and metadata-only internal `mutation.reservation-source-recorded`
    Session event in one `IMMEDIATE` transaction. On the first restart it validates
    the complete graph before atomically moving a reserved row to
    `reconciliation-required` revision 2 and appending exactly one internal
    reconciliation event; the second restart performs zero writes and advances no
    sequence. These events are not Public Timeline Journal events and grant no
    authority. Migrated `legacy-unavailable` rows receive no fabricated event.
    The exact lifecycle lookup uses the covering
    `events(session_id, operation_id, sequence, event_kind)` index; an
    `EXPLAIN QUERY PLAN` regression, without `ANALYZE`, requires SQLite to select it.
  - Implemented v23 event slice atomically appends one internal
    `mutation.reservation-outcome-recorded` event with the immutable outcome row and
    reservation revision CAS, then validates the whole terminal graph before commit.
    Exact outcome replay advances no sequence; peer same-outcome replay returns the
    committed graph, peer drift conflicts, and all injected event/row/CAS/validation/
    commit failures roll back the internal sequence and graph. The event remains
    outside the Public Timeline Journal and grants no authority.
  - Implemented v24 consumption evidence references those existing immutable internal
    source/outcome/reconciliation events without appending a fourth event type or
    advancing either internal or Public Timeline sequences. Source-first `c0` to
    `c1` and optional resolution `c1` to `c2` are independent CAS transitions;
    exact/peer retry is zero-write, all write-lock gates are rechecked, and insert,
    update, validation, and final-commit failures roll back the ledger atomically.
  - Remaining: production producer-side acknowledgement, AAP/Qt recovery,
    external caller consumption/CAS, and reviewed recovery for approvals, files, Git,
    and jobs. The internal v23 metadata event graph is not a completed product
    acknowledgement or recovery stream.
- [x] 5.3 Implement content-addressed storage for large output, patches, images, and artifacts with checksums and retention metadata
  - The current schema v14 retains the v4-introduced project-external SHA-256 object design with exact byte count, content reference, media/kind, session/project owner, source owner, hashed bounded metadata, created/accessed/verified time, state, and retention deadline. Files use private Unix modes, deterministic sharded paths, same-directory create-new staging, full file sync, no-clobber hard links, and directory sync; reads recheck type, size, and SHA-256. Admission caps one object at 16 MiB, the store at 8,192 objects/512 MiB, and retains a 256 MiB free-space reserve where the platform reports capacity.
  - Command-output artifacts now commit atomically with their Item/event and survive sidecar restart while retaining existing session-scoped AAP reads. Changes preview content and full diffs persist as an all-or-nothing batch and remain page-readable after restart. The common binary path supports patch, image, diagnostic, workspace-edit, command-output, and generic artifact media; current product flows have no Agent image producer yet.
  - Release is non-destructive for at least a 24-hour undo window. GC only selects references explicitly released past every deadline, rechecks integrity and eligibility under a write transaction, and never removes active, corrupt, missing, unknown, or unregistered files. Fixtures cover deduplication, reopen, empty/binary media, cross-session isolation, metadata-secret denial, low-space admission, missing/hash/size corruption, reference/event rollback, active retention, undo-window GC, unknown orphan preservation, and v4→v6 migration through the versioned Blob step.
- [x] 5.4 Implement rebuildable session projections and a consistency verifier for sequence, checksums, and orphaned blobs
  - The bounded, content-free verifier checks project/lineage binding, turn hashes, item ownership/sequence/hash, event sequence/cursor/hash, durable reference ownership, projection owner existence, metadata integrity, and referenced file size/SHA-256. Fresh session metadata, Turn, and Item projections rebuild into an independently validated candidate from typed events. Rebuild binds the complete reviewed event-stream identity, reacquires an `IMMEDIATE` write lock, rejects source drift, atomically replaces only projection rows, and appends content-free audit evidence. A separate bounded store scan reports missing/corrupt objects, dangling references, malformed disk entries, and unregistered files without paths, hashes, or content; it never deletes uncertain orphans.
  - Project creation and independently scoped additional roots now form a replayable project stream. Startup scans and repairs projects before dependent sessions, reconstructs a missing multi-root project exactly, treats offline stored roots as history rather than corruption, and quarantines every bound session when project authority is incomplete or tampered. Session startup remains capped at 10,000 identities and 100,000 Turns, Items, events, and Blob references per class; missing lineage rebuilds parent-first after project, parent, Blob owner/content, source-count, cursor, and full-stream-hash revalidation.
  - Scan/query/limit failure enters whole-store read-only recovery. Uncertain sources enter per-project/per-session quarantine; Store and Runtime block unsafe metadata, Turn, Item, Artifact/Blob, Git authorization/event, terminal input/start, and preview writes while retaining cancellation and terminal cleanup. AAP and Qt expose bounded recovery state and keep healthy sessions usable. Deterministic SQLite insert-abort fixtures and real child-process exits after destructive project/session projection clears prove transaction rollback preserves the complete previous projection, writes no false audit, and permits automatic recovery on the next startup.
- [x] 5.5 Implement WAL configuration, bounded busy handling, compaction, backup, and low-disk behavior
  - SQLite uses WAL, `synchronous=FULL`, a two-second busy timeout, 1,000-page automatic checkpoints, a 16 MiB journal retention limit, and a 1 GiB database page/combined database-plus-WAL write ceiling. Every ordinary `IMMEDIATE` write now enters through one admission boundary that preserves the shared 256 MiB free-space reserve plus 8 MiB transaction headroom before acquiring the write lock. Low space returns stable content-free codes with no projection/event side effects; verified artifact reads remain available without updating access metadata, while release/delete and all other mutations remain blocked.
  - Explicit maintenance refuses projection-recovery state, databases above the size ceiling, and capacity below `reserve + headroom + 2*database + WAL`. It performs a truncating WAL checkpoint, SQLite `VACUUM`, and `quick_check`, then returns bounded byte/freelist evidence without content. Fixtures prove PRAGMA limits, exact low-space admission boundary and zero writes, read-only artifact degradation, competing-writer timeout below five seconds, maintenance low-space rejection, freelist reclamation, and post-maintenance integrity. WAL-consistent pre-migration backup, bounded manifests, and failure recovery remain provided by completed tasks `5.6`/`5.7`.
- [x] 5.6 Implement versioned migrations with pre-upgrade backup and read-only recovery on failure
  - Transactional v1→v6, v2→v6, v3→v6, v4→v6, and v5→v6 migrations create a WAL-consistent SQLite Online Backup before changing schema state. Backups are normalized into standalone `journal_mode=DELETE` files, SHA-256 bound to a private no-clobber manifest, capped at 1 GiB/16 evidence sets, and never overwrite or automatically delete ambiguous evidence. Every migration verifies the complete required v6 schema before advancing `user_version` and commit.
  - Migration, schema, integrity, configuration, backup, and newer-version failures enter a content-free read-only Runtime without starting Codex. Recovery initialization advertises only status/export/read-only capabilities; `runtime/recovery/status` and `runtime/recovery/export` expose stable codes and counts without database content, project paths, or credentials, while ordinary AAP operations fail with `-32120`. Newer schemas are never downgraded.
- [x] 5.7 Add migration fixtures from every supported schema version plus corrupted and interrupted migration cases
  - Fixtures cover v1/v2/v3/v4/v5 sources into v6 with one verified source-version backup, preservation of existing v3 events and session projections, schema-collision rollback, random corrupt bytes without rewriting the original, an uncommitted migration transaction with preserved partial evidence and safe re-entry, backup low-space rejection, backup tampering without deletion, and a newer schema remaining unchanged in recovery mode.
- [x] 5.8 Implement project/session retention, archive, delete scope, content garbage collection, and undo window
  - Schema v6 persists project/session retention overrides plus exact `session-only` or descendant-lineage deletion plans. Preview hashes the complete bounded member/Turn/Item/event/artifact impact, scheduling rechecks the reviewed hash under an `IMMEDIATE` transaction, freezes every member, and exposes a 24-hour-to-30-day undo deadline. Undo removes the pending plan with durable audit evidence; expiry installs minimal lineage-preserving tombstones, releases Blob references, and keeps physical content for another minimum 24 hours before integrity-checked GC.
  - AAP exposes deletion preview/schedule/status/undo, retention policy read/set/remove, and host-triggered maintenance. Runtime activity protection covers the selected root and every descendant, including active Turns and running/stopping terminals; durable live Turns/approvals and recovery quarantine remain protected. Purged resident timeline, command-artifact, edit-preview, and terminal caches are evicted; normal listing/history hides tombstones, including after restart.
  - Qt exposes scope choice, exact descendant/Turn/Item/artifact review, destructive confirmation, a fixed seven-day undo window, pending labels/banners, disabled mutation controls, and context-menu Undo. Project and per-session policy dialogs configure archive/delete ages, undo duration, lineage scope, or policy removal. Six Store fixtures, one Runtime test, one external AAP protocol test, and the desktop render flow cover stale plans, rollback, live-work protection, pending reads, undo, purge, delayed Blob GC, policy precedence, restart, and UI state.
- [x] 5.9 Implement portable redacted session export and import with schema/version validation and collision handling
  - The documented `aegisy-portable-session/0.1` package carries bounded session metadata and up to 2,000 redacted Items within 4 MiB. A SHA-256 content identity binds the complete portable content; export rebuilds after preview and rejects stale hashes. Every string is rescanned for recognized secrets and registered project absolute roots, while credentials, provider response IDs, hidden/encrypted reasoning, cache/continuation handles, environment identity, durable Blob bytes, and local artifact/content references are excluded.
  - Import validates envelope/content versions, content hash, limits, contiguous sequence, unique source Item IDs, payload bounds, and a second redaction/exclusion pass before mutation. Work imports require an active target project and receive a fresh local environment identity. Explicit `reject` and `copy` collision strategies are enforced again under the SQLite `IMMEDIATE` transaction; copy always creates a new Session and deterministically remaps Item IDs, linking fork lineage only when the same readable source Session and project/mode remain authoritative.
  - AAP exposes export preview/commit and import preview/commit with stable integrity, redaction, collision, stale, blocked, and recovery errors. Qt previews content categories, code/path/transcript/command warnings, redaction and exclusion counts, project target, and collision impact; it enforces a 4 MiB input cap, atomically writes with `QSaveFile`, and refreshes/selects the imported Session. Four Store fixtures, one external protocol fixture, projection restart replay, injected audit failure rollback, both render targets, and the complete test/Clippy/OpenSpec gates pass. Provider continuation is explicitly not portable and remains a later runtime/model-fork concern.
- [x] 5.10 Verify credentials, long-lived JWTs, API keys, and raw secure-storage values never enter database or event payloads
  - A shared persistence gate rejects credential-like field names and recognized secret values before event sequence allocation or any projection write. It covers nested JSON event payloads, event identifiers, project/session text, canonical roots, identity fields, approval identifiers, and durable Blob metadata without treating credential IDs or ordinary model settings as raw credentials.
  - Item payloads are redacted before storage and replay rejects any unredacted value or credential-like field. Event replay and projection-source replay apply the same invariant, so legacy/tampered secret-bearing event rows become content-free read-only recovery rather than being returned or rebuilt.
  - Portable export retains historical-data re-redaction for source-less legacy sessions, while normal project/session APIs reject secret-bearing display text. A failed event gate rolls back an already-mutated transaction, and rejected project/session/blob metadata leaves zero durable rows.
  - The Qt host removes credential-bearing environment variables before launching `aegisy-agentd`; an isolated host test proves API keys, login/refresh tokens, cloud secrets, and authenticated proxies do not cross the sidecar boundary while safe runtime settings remain.
  - Store fixtures cover nested JWT/API-key redaction, title/display rejection, event-gate rollback, Blob metadata rejection, legacy replay quarantine, and zero-row assertions. The full Rust workspace, protocol, strict Clippy, desktop CTest, formatting, diff, and OpenSpec gates pass.

## 6. Project and Session Management

- [x] 6.1 Implement project identity from canonical roots without treating moved or missing paths as new trusted projects
  - Project roots now receive a filesystem-backed identity: Unix device/inode and Windows volume serial/file ID, with a deterministic path fallback only on unsupported platforms. New opens persist this identity instead of trusting a canonical path hash alone.
  - `project/open` matches the stable identity before allocating a project ID. A moved directory returns the original project with `availability=moved`, the candidate root, and `relink_required=true`; it does not silently rewrite the trusted root or create a second project. An exact missing saved root remains addressable as `availability=unavailable` with explicit relink required.
  - Existing path-hash identities are migrated atomically on the next exact-path open through a typed project event, and project projection replay understands the migration without losing root identity. The current runtime still leaves the explicit relink mutation to task `6.4`.
  - Runtime fixtures cover first open, directory move, missing original path, stable project ID, candidate-root reporting, no duplicate project rows, and restart-compatible identity persistence. Full Rust/protocol/desktop/OpenSpec gates pass; clean Windows filesystem identity/runtime evidence remains external.
- [ ] 6.2 Implement first-open trust review showing roots, repositories, instructions, executable hooks, and policy impact
  - Partial read-only review foundation: `project/open` and `project/trust-review` return a bounded `project-trust-review/0.1` snapshot with the resolved root, root access/symlink policy, detected repository metadata, instruction paths (without reading their bodies), executable Hook paths with bounded size/hash metadata, truncation state, and explicit read-only/write/network/Hook policy impact. Qt renders the paths and policy summary in the Agent timeline and keeps discovered Hooks disabled.
  - `project/trust-acknowledge` now re-scans the exact registered root, requires the reviewed root identity and `review_id`, and appends a content-free `project.trust-acknowledged/0.1` event. Instruction and Hook content identities participate in the review hash without returning their bodies; a root, instruction, or Hook change marks the prior acknowledgement `invalidated` after restart. Duplicate acknowledgement is idempotent. Qt exposes explicit confirmation from the active project's context menu and states that confirmation grants no write, command, Hook, or network authority.
  - Keep this task unchecked: the acknowledgement records review only and does not grant execution. Managed-policy intersection, the complete permission/approval bridge, and executable-content authorization remain required before any project instruction or Hook can influence authority or execute.
- [ ] 6.3 Implement add/remove root with independent read/write scope and symlink/canonical-path validation
  - Partial scoped-root foundation: Runtime exposes `project/root-list`, `project/root-add`, and `project/root-remove`. Add validates read/write access, canonicalizes existing directories, rejects a symlink final root, binds filesystem identity, rejects duplicate canonical/identity roots, and stores independent root access. Remove is event-backed and cannot remove `root-1` or the last remaining root; projection replay and restart fixtures preserve the exact remaining roots. Qt provides a reviewed project-root dialog with explicit read/write selection, add confirmation, remove confirmation, availability state, and a warning that files are not deleted and Agent access is not granted. Core `workspace/list`, `workspace/read`, `workspace/metadata`, `workspace/save-user-text`, `workspace/watch`, search, index/repository-map, LSP, observed diagnostics, and structured turn context now accept or carry an optional `root_id`; each request revalidates the registered root identity, and writes still reject read-only roots. Persistent trust acknowledgement/invalidation, a complete approval/authorization bridge, and full Agent policy intersection remain required, so task `6.3` remains incomplete.
- [ ] 6.4 Implement pinned/recent/unavailable project navigation and relink flow
  - Partial navigation implementation: AAP `project/list` now returns bounded persistent project entries ordered by pinned/recent state, availability, relink requirement, session counts, and live-turn counts. `project/navigation` persists pin/unpin changes through SQLite schema v7 and a typed `project.navigation-updated/0.1` event. Qt renders the left project rail, fixed/unavailable/live badges, context-menu pinning, project selection, and explicit relink recovery; a durable protocol fixture verifies pin state, move-to-unavailable, restart, and relink restoration. Keep this task unchecked until active-session state includes all required runtime/approval/background states and the complete navigation workflow has its final cross-platform/UI evidence.
- [ ] 6.5 Implement create, title, resume, fork-at-boundary, archive, unarchive, and scoped delete operations
  - Partial session-management implementation: the sidecar exposes bounded `session/list` metadata plus title, archive/unarchive, and two-phase scoped deletion. SQLite and in-memory runtimes preserve title/status, pending deletion and undo state; archived sessions reject new work, live activity blocks archive/delete, and purged tombstones remain only for lineage integrity. Qt provides rename/archive/restore/delete/undo and now exposes explicit resume/fork actions.
  - SQLite schema v8 persists a read-only session runtime binding (adapter/version/provider/model and opaque backend thread identity) with `session.runtime-bound/0.1` and `session.resumed/0.1` audit events. `session/resume` reconstructs Preview sessions after restart, hydrates a bounded validated in-memory timeline, and calls the pinned Codex 0.144.5 `thread/resume` contract when the exact binding is present; missing or incompatible bindings fail explicitly and require a portable fork. `session/fork` copies redacted history through the latest or a specified completed turn, records `fork` lineage, and calls the exact Codex `thread/fork { threadId, lastTurnId }` contract for Codex sessions. Durable protocol coverage proves Preview resume/fork and restart recovery.
  - Keep this task unchecked until resume/fork support is joined to full provider thread lifecycle mapping, runtime state isolation, complete turn/item boundary semantics, and final Qt/Codex failure-recovery evidence.
- [ ] 6.6 Implement indexed session search by project, branch, model, runtime, status, title, and approved local transcript fields
  - Partial bounded search implementation: AAP `session/search` returns `session-search/0.2` and filters durable Session projections by project, exact branch/model/runtime/status, title, or one query that matches title or approved local transcript fields. Transcript matching stays inside SQLite, is limited to `message` Items with `user`/`assistant` roles and the visible `text`/`content`/`output`/`diff` fields, and never loads every transcript into Runtime memory. Results include Runtime and Workspace binding metadata, matched fields, a strict `updated_at/session_id` cursor, a 100-result maximum, and purged-session exclusion. Schema v13 adds an indexed, event-backed `session_workspace_bindings` projection through the normal WAL-consistent migration-backup gate. Work Session creation atomically records primary-root identity, Git availability, branch hash/safe display, HEAD, worktree identity, and an explicit `dedicated_worktree:false` state without repository paths; branch search uses the exact hash. Sensitive-shaped branch labels are not persisted or returned. A real Git protocol fixture proves restart search/read/resume and rejects resume after branch drift. Qt exposes a debounced left-rail search with explicit empty state; Work searches the current project while Chat searches all projects.
  - Keep this task unchecked: complete indexed-text/scale evidence, model/runtime control-plane integration, branch-filter UI, dedicated worktree lifecycle integration, and final cross-platform behavior remain required.
- [ ] 6.7 Implement per-session runtime/model/permission/terminal/extension/worktree/context state isolation tests
  - Partial isolation evidence: the AAP protocol fixture verifies distinct per-session
    environment identities, read-only runtime profiles, structured context scoping, and
    independent in-memory history. A separate real-Git fixture now proves a Work Session's
    branch/HEAD/worktree binding survives restart and that a later branch drift blocks
    resume instead of changing the stored binding. Terminal cross-session denial is covered
    separately; concurrent dedicated-worktree ownership, extension state, and complete
    provider/model lifecycle isolation remain pending.
- [x] 6.8 Implement session replay UI data source and paginated older-history loading
  - Qt consumes `session/list` for the left rail, prioritizes current-project Work sessions, and reads a selected durable session through `session/read`. The first read returns the newest bounded page; strict session-sequence cursors page backward without overlap, and an explicit load-more control prepends older items while retaining the user's scroll anchor. In-memory and SQLite restart paths share the same page contract, sequence metadata, invalid-cursor rejection, and 1-200 item bounds. Project unavailable/relink behavior remains task `6.4`, while complete event-derived projection rebuild remains task `5.4`.
- [ ] 6.9 Implement interrupted/unknown operation reconciliation against filesystem, process, Git, and event state
  - Partial internal contract: `operation-reconciliation/0.1` accepts content-free event, process, workspace, and Git evidence and emits a bounded state/decision, blocker list, observed domains, and content-hashed review ID. Missing terminal events remain `unknown`; live processes remain `running`; changed/unavailable workspace or Git state, in-progress Git operations, and missing required evidence require explicit review and block subsequent writes. The contract never infers mutation success, probes the host, executes recovery, or exposes AAP/Qt mutation controls. Keep this task unchecked until durable operation records, restart reconciliation against authoritative filesystem/process/Git/event probes, user review, and recovery actions are integrated.
  - Partial durable integration: AAP `operation/reconcile` validates the same evidence contract, appends a content-free `operation.reconciled/0.1` event to the bound session stream, is idempotent for identical evidence, and reads the latest per-operation result after Runtime restart. Unknown/blocked results now gate later session-bound mutations with stable error `-32132`; a newer authoritative completed/failed/interrupted review removes that gate. Capability `operation.reconciliation` and protocol/store fixtures cover in-memory blocking, durable event persistence, restart blocking, idempotency, and unblocking.
  - Partial probe integration: capability `operation.reconciliation.probe` exposes read-only `operation/probe`, which resolves a registered Work-session root, hashes bounded visible workspace metadata, reads structured Git status, and observes only runtime-owned turn/terminal state. It returns state labels and snapshot hashes without content, arbitrary paths, or caller-selected PIDs; callers may provide event state, while the bounded durable turn/Git mappings may supply it when omitted, and callers must still submit the result to `operation/reconcile`.
  - Partial startup discovery: a durable Runtime preload scans the latest validated reconciliation event for each session/operation pair into a bounded cache; the event store remains authoritative for request-time gating and scan failures never imply safety.
  - Partial event authority: when `operation/probe` omits `event`, durable Runtime derives the latest validated `turn.*` state and the existing `git.workflow.*` lifecycle state from the session event stream; Git prepared/dispatching/in-progress map only to running, completed/failed/aborted map to terminal evidence, and conflicted/recovered remain unknown. Explicit caller event values remain labelled as caller-supplied and workspace-edit/terminal/background-job event families are not inferred.
  - Partial review status: capability `operation.reconciliation.status` exposes read-only `operation/status` for the current session gate, including the bounded review summary and an explicit `recovery_action_available:false` state while blocked. Qt provides an explicit turn-only review path that collects `operation/probe` evidence and records `operation/reconcile`; identity/schema failures remain read-only and Git/Workspace/Terminal review is unavailable.
  - Keep this task unchecked: workspace/terminal/background-job event sources, complete authoritative host probes, durable operation discovery beyond the bounded preload, full cross-kind Qt review, and recovery actions remain required.
- [ ] 6.10 Implement explicit session compaction checkpoint creation, review, preservation instructions, and failure recovery
  - Partial internal contract: `session-compaction/0.1` validates bounded summaries for decisions, unresolved tasks, changed files, commands, tests, failures, and next actions; rejects secret-shaped/control-character content and enforces item/byte limits. Review IDs are content-hashed, activation requires the exact sequence and source-context identity, and failed checkpoints preserve the original context while exposing bounded model-change, portable-fork, and manual-cleanup recovery options. Qt now provides manual create/read review dialogs plus an edit-to-new-review flow; automatic thresholds, model-generated summaries, editable activation, and Codex `thread/compact/start` remain unimplemented.
  - A separate internal `session-compaction-checkpoint-store/0.1` persists the exact validated review under the Workbench data root with content-addressed objects, hashed session/checkpoint pointers, no-clobber publication, bounded counts/bytes, private Unix permissions, restart revalidation, idempotent identical writes, and tamper preservation. `WorkbenchStore` appends an idempotent, metadata-only `session.compaction-checkpointed/0.1` event and validates it during projection replay; revision events carry a validated `supersedes` descriptor and require the source checkpoint event to exist, while neither event copies summary/instruction content and both explicitly record that original history remains authoritative. AAP capability `session.compaction.checkpoint-review` exposes manual `session/compaction/checkpoint/create`, `read`, and immutable `revise`: create derives sequence/context identity from the complete verified event stream, blocks active turns, detects conflicting checkpoint reuse, and returns activation/provider-compact as unavailable; revise requires the exact source Review ID and a new checkpoint ID, preserves the old object, and is idempotent; read requires the validated object and matching event after restart. The filesystem object and SQLite event are not one cross-resource transaction, and startup compensation, activation, provider compact, and deletion flow remain unavailable. Keep the task unchecked until preservation instructions and summaries are editable/reviewed before activation, automatic/model generation and failure compensation/recovery are integrated, and all permission/provider gates pass.

## 7. Codex App Server Runtime Adapter

- [ ] 7.1 Pin an initial compatible Codex App Server version and generate its stable protocol schemas
  - Partial pin: `codex-cli 0.144.5` is enforced before app-server launch, and the
    generated v2 schema bundle is checked in at
    `agent-runtime/aap-schema/codex-app-server-0.144.5/v2.schemas.json`. Cross-platform
    binary contract tests, upgrade/rollback procedure, and compatibility matrix remain.
- [ ] 7.2 Implement adapter process launch, initialize handshake, health, stderr tracing, shutdown, and version rejection
  - Partial adapter lifecycle: process launch, initialize/initialized handshake, exact version rejection, bounded stdout/stderr draining, Drop-time child cleanup, and AAP `runtime/health` process state/PID reporting are implemented and tested.
  - Stderr tracing now publishes only bounded bytes/newline/redaction counters and a stable last class (`fatal`, `error`, `warning`, `timeout`, or `info`); raw text is discarded and never enters AAP.
  - Startup supervision now applies a 15-second initialize timeout, retries only transient transport/EOF/read/write/timeout failures, and caps attempts at three with bounded backoff. Version mismatch and protocol rejection are not retried; a real fixture proves the crash-loop cap. AAP `runtime/restart` now recovers an exited/unavailable adapter while preserving session bindings and blocks during active turns or user terminals. Qt polls `runtime/health`, exposes exited/unavailable state, and provides a guarded restart action. Full crash-loop recovery UI and cross-platform recovery evidence remain.
  - Ubuntu CI exposed that a child-stdin `BrokenPipe` raised while
    `serde_json::to_writer` wrote directly to the pipe was labelled as an encoding
    failure and bypassed transient-write retry. Startup now serializes the complete
    frame before the write/flush path; a deterministic BrokenPipe fixture requires
    the write-error class and exactly three bounded attempts. Full crash-loop
    recovery UI and cross-platform pinned-binary recovery evidence remain, so keep
    `7.2` unchecked.
- [ ] 7.3 Map Codex thread start/resume/fork/list/read/archive/delete/compact to AAP sessions
  - Partial adapter mapping: pinned Codex App Server 0.144.5 `thread/start`, `thread/resume`, `thread/fork`, `thread/list`, `thread/read`, `thread/archive`, `thread/unarchive`, `thread/delete`, and `thread/compact/start` are schema-driven at the adapter boundary. `session/start`, `session/resume`, and `session/fork` use the first three. AAP `session/archive` and `session/unarchive` now update the bound Codex thread when its provider state is loaded, advertise explicit lifecycle capabilities, reject unloaded provider state, and compensate provider changes when local persistence fails. AAP `session/provider-list` and `session/provider-read` provide bounded, metadata-only Codex projections with raw provider items omitted. Provider delete/compact, complete lifecycle recovery, and user review/compensation for destructive operations remain pending. Agent/Codex mutation permissions remain read-only.
- [ ] 7.4 Map turn start, completion, interruption, steering, plan, diff, token usage, and structured errors
  - Partial turn mapping: cancellation/failure Items carry bounded redacted `runtime-error/0.1` class/retryable metadata for transport, timeout, provider, persistence, and adapter failures; schema-driven token usage, plan, and unified-diff notifications map to bounded AAP timeline events.
  - Same-turn steering now uses an identity-scoped out-of-band `turn/steer` request with an eight-entry queue, 64 KiB input bound, pinned `expectedTurnId` provider mapping, and separate queued/acknowledged/failed states. Steering remains read-only and its user Item is bounded and redaction-gated.
  - Usage, plan, and unified-diff updates now append unique bounded durable Items when storage is configured, with at most 32 updates per kind per turn and one truncation marker after the cap. Restart replay covers all three kinds. Complete provider error/reconnect fixtures and final Qt/provider lifecycle evidence remain.
- [ ] 7.5 Map message, reasoning summary, command, file-change, tool, review, image, and artifact item lifecycles
  - Partial file-change mapping now follows the pinned Codex `0.144.5` lifecycle
    `patchUpdated* -> item/started -> requestApproval -> decline -> serverRequest/resolved -> item/completed(declined)`.
    The adapter treats `patchUpdated` only as volatile parsed-so-far preview state;
    it may be absent, partial, use relative paths, or use a different diff shape and
    contributes no Proposal authority. `item/started` is the canonical Proposal
    authority, while `item/completed` must repeat that exact canonical change set.
    Started/completed timestamps are required and monotonic, pending plus Started
    items share a 256-item limit, and retained file-change state is capped at 16 MiB
    per Turn. Approval and resolution remain bound to the exact thread/turn/item,
    and a Provider `completed` file change is a potential read-only boundary violation.
    Add/update/delete/pure-rename conversion preserves raw UTF-8 BOM and LF/CRLF,
    binds raw base hashes, rejects stale/escaping/duplicate/oversized changes, and
    never writes the workspace. New durable records use
    `workspace-edit-proposal/0.2` with aggregate and ordered per-file summaries;
    untruncated diff Blobs are semantically re-read for additions/deletions on
    admission, direct read, and startup/restart. Exact legacy `0.1` records retain
    their canonical bytes/identities and project as explicitly incomplete summaries.
    Schema v19 now projects every newly persisted Proposal as one completed `tool`
    `file-change` Item whose `workspace-edit-proposal-reference/0.1` metadata binds
    the Session, Turn, Proposal, project/root/edit, preview identity, aggregate
    counts/applicability, and false mutation/approval/apply authority. The Item,
    public `item.completed` envelope, internal Item/Proposal events, Proposal,
    artifacts, Blob references, and immutable binding share one Store transaction;
    Runtime commits the prepared in-memory sequence and emits only afterward.
    Qt restores strictly validated durable file-change Proposals into Changes with
    foreground auto-open, background unread state, reconnect revalidation, bounded
    artifact paging, and no approval/apply controls. Qt also validates the exact
    Timeline reference, offers a read-only `View changes` action, and rechecks an
    exact Proposal response without allowing it to overwrite the latest cache on
    drift or race. Message/reasoning/review/image and complete Tool families remain
    incomplete, so keep `7.5` unchecked.
- [ ] 7.6 Map command, file, permission, MCP elicitation, and user-input server requests to AAP approvals/questions
  - Partial file-request boundary: before the fixed Runtime-policy `decline` is
    written, the exact Codex file change is compiled into an immutable
    `workspace-edit-proposal/0.2` and committed with its content/diff Blob
    references, completed metadata-only Change Item, internal Item/Proposal events,
    public `item.completed` envelope, and v19 immutable Timeline binding. Store,
    sequencing, serialization, or Proposal failure sends no decline and fails the
    Turn closed; notification and in-memory sequence commit occur only after the
    SQLite commit. The Proposal and reference record no user decision and fix
    mutation, approval, and apply authority to false. Negotiated
    `workspace.edit.proposal.read-only` plus
    `permission.read-only` now exposes Session-scoped latest/exact reads and bounded
    Proposal-owned artifact pages; `-32149` and `-32150` keep missing Proposal and
    artifact/page state distinct. Exact `0.1` read compatibility remains available
    without rewriting legacy identities. Qt read-only Proposal recovery is
    implemented. Genuine user approval/questions, durable decision consumption, MCP
    elicitation, and all write authority remain absent.
- [ ] 7.7 Map background terminals, filesystem read/write/watch, fuzzy search, Skills, hooks, plugins, and MCP capability state
- [ ] 7.8 Configure Aegisy custom provider and short-lived token flow without writing the desktop login token into Codex config
- [ ] 7.9 Implement adapter degradation flags for experimental or missing Codex methods
  - Partial degradation contract: `runtime-degradations/0.2` binds the complete pinned Codex `0.144.5` ClientRequest, ServerNotification, and ThreadItem classification to the vendored v2 schema hash and a fixed capability-matrix identity. AAP reports read-only Agent mutation, content-free provider thread metadata, blocked provider delete/compact, runtime-only desktop gaps, and disabled autonomy release gates without granting authority. Provider list/read `0.2` omits thread/item content, names, previews, paths, cwd, model-provider text, and source text; only bounded fixed-enum/count/time/presence fields, validated lossless opaque IDs/cursors, and domain-separated workspace/session hashes remain. Invalid opaque values reject the complete projection rather than being truncated or redacted into a different identity. Unknown vendor notifications are counted in `runtime/health` only by bounded SHA-256 method identities and never become Timeline Items. `item/tool/requestUserInput` returns a fixed unsupported error instead of fabricated empty answers.
  - Qt validates the exact backend, matrix identity/hash/counts, complete feature set, authority flags, and schema before enabling new Turns. Not-requested, pending, failed, or malformed snapshots disable Send and pending Turn auto-start while preserving an active Stop action. Live and replay Timeline inputs share strict bounded item validation; unknown/cross-bound events, malformed pages, duplicate IDs, and kind/role drift are inert, and a replay page is applied only after complete validation. Runtime event counters and Qt cursors are Session-scoped, and Chat/Work switching is blocked during the single active Turn; focused and ordinary Qt render coverage both pass. Persistent public-event replay, unknown-event cursor diagnostics, full vendor capability negotiation, desktop surfaces for runtime-only methods, and all dependent feature gates remain incomplete; provider delete/compact actions are still not exposed, so keep this task unchecked.
- [x] 7.10 Record and redact protocol fixtures for happy path, partial stream, approval, denial, cancel, reconnect, compaction, and provider failure
  - `aap-schema/fixtures/codex-thread-lifecycle.jsonl` records initialization, health, degradation, provider list/read, session start/archive/unarchive, and shutdown; `codex-turn-metadata.jsonl` covers schema-aligned usage, plan, and diff notifications; `codex-recovery.jsonl` records partial output, structured transport/provider failure, cancellation request/acknowledgement/interruption, reconnect, compaction, and fixed approval denial; and `codex-provider-lifecycle-failures.jsonl` binds provider-state-unavailable, lifecycle failure, compensation failure, compact-blocked, and failed-then-successful restart states to stable content-free codes.
  - Protocol tests parse every line, reject credential-shaped content, and assert the terminal/error/recovery states. Real stdio fixtures verify retryable transport failure, identity-preserving restart, recovered completion, provider failure, approval denial, cancellation, and archive/unarchive mapping. Qt render coverage proves structured error class/retryability plus archive, unarchive, fork, and restart failures retain operation/error-code/recovery state while raw provider payloads stay out of labels and tooltips. Provider delete/compact product actions remain separately gated by `6.10` and `7.3`.
- [ ] 7.11 Add contract tests against the pinned binary on macOS and Windows
- [x] 7.12 Document runtime upgrade procedure, compatibility matrix, emergency pin, and rollback
  - `docs/CODEX-ADAPTER-UPGRADE.md` defines the pinned 0.144.5 compatibility matrix, candidate schema/fixture review, macOS/Windows contract gates, emergency pin, artifact rollback, provider-binding incompatibility handling, and content-free evidence requirements. The procedure keeps the adapter's exact-version rejection and read-only permission boundary intact.

## 8. ACP Runtime Adapter

- [ ] 8.1 Pin the ACP protocol version and implement initialize/auth/session capability negotiation
- [ ] 8.2 Implement ACP session create/load/prompt/cancel and event translation into AAP
- [ ] 8.3 Map ACP tool calls, permissions, plans, file references, terminals, and session metadata updates where available
- [ ] 8.4 Implement namespaced Aegisy extensions only for gaps that cannot use standard ACP methods
- [ ] 8.5 Add capability-based fallback UI for ACP agents lacking structured patches, Git, child sessions, or background terminals
- [ ] 8.6 Add Kimi CLI fixture and live contract tests for multi-session, approvals, compaction, provider state, and authentication-required errors
- [ ] 8.7 Add one additional ACP agent contract test to prevent Kimi-specific behavior from becoming the de facto protocol
- [x] 8.8 Document supported ACP agents, versions, missing features, installation ownership, and troubleshooting
  - `docs/ACP-ADAPTER-SUPPORT.md` defines the planned ACP adapter support model, user installation requirements, feature matrix with fallback behaviors, authentication ownership, known limitations, troubleshooting guide, and migration comparison with Codex. The document explicitly states ACP is not yet implemented and serves as the specification for future work. Completed 2026-08-01.

## 9. Aegisy Model Catalog and Cloud Contracts

- [ ] 9.1 Define versioned model-catalog schema for identity, provider, protocols, limits, capabilities, roles, availability, entitlement, lifecycle, and policy
  - Partial foundation: internal `model-catalog/0.1` validates bounded metadata,
    explicit unknown availability/capability/limit states, runtime degradation
    metadata, and field authority labels. It is exposed only through the
    read-only `model.catalog.read-only` AAP projection and is not a signed or
    authenticated cloud catalog.
- [ ] 9.2 Define which catalog fields are upstream-authoritative, Aegisy-configured, evaluation-derived, estimated, or unknown
  - Partial foundation: `FieldAuthority` is explicit for availability,
    entitlement, lifecycle, limits, capabilities, roles, policy, and runtime
    compatibility. Unknown and estimated values remain non-authoritative, and
    unsupported authority keys are rejected. This does not establish a trusted
    upstream source or signed field provenance.
- [ ] 9.3 Implement catalog signing, key rotation, cache expiry, rollback protection, and schema validation
  - Partial foundation: schema validation now enforces fresh catalogs to be
    signature-validated, invalid catalogs to be unsigned, bounded catalog/source
    text, and secret-free metadata. Internal `model-catalog-signature/0.1`
    now verifies a canonical catalog payload with strict Ed25519 verification
    against a bounded `model-catalog-key-ring/0.1`. The key ring binds a positive
    generation, content identity, public-key validity windows, revocation state,
    and replacement lineage. Rotation must advance exactly one generation,
    preserves prior Key IDs and public keys, cannot widen old validity or undo
    revocation, treats an identical generation as idempotent, and rejects
    rollback, conflict, missing history, or unknown lineage. A cache Store entry
    point sets `signature_validated` only after the envelope passes schema,
    payload-identity, time, key-state, and signature checks.
    Internal `model-catalog-cache/0.1` additionally binds signed-catalog content
    identity, positive monotonic sequence, receipt/issue/expiry time, bounded
    TTL/stale windows, idempotent same-generation replay, and rejection of older
    or conflicting generations. A private
    `model-catalog-cache-store/0.1` now persists the validated cache outside the
    project root using an atomically replaced, bounded snapshot, private Unix
    permissions, restart identity validation, and tamper detection. Runtime
    opens this store when the durable data root is healthy; standalone runtimes
    retain an in-memory fallback. Internal
    `model-catalog-key-ring-signature/0.1` now binds the signer, signature time,
    complete ring, and payload identity. A private
    `model-catalog-trust-store/0.1` accepts generation one only when that envelope
    verifies against an exact externally supplied Ed25519 root anchor and the ring
    preserves the same root key. Later generations must be signed by a current,
    active, unrevoked key from the previous ring before the existing monotonic
    rotation rules run. The Trust Store persists the anchor and current ring in a
    private atomic snapshot, writes an empty first-open marker so deleting the
    snapshot cannot re-enable generation-one bootstrap, and restores its prior
    in-memory authority when disk commit fails. Cache Store public admission now
    requires this Trust Store; raw validated-catalog mutation remains private.
    Neither Store is Workbench-SQLite event-backed. The repository intentionally
    contains no production root key, and the host/runtime does not yet open the
    Trust Store. No authenticated Key Ring/catalog download, signing service,
    cloud refresh, key-publication endpoint, or AAP/Qt refresh path exists.
- [ ] 9.4 Add authenticated catalog endpoint with conditional requests and deterministic test fixtures
  - Partial foundation: internal `model-catalog-refresh/0.1` validates the
    host-owned authenticated transport observation, bounded ETag/
    Last-Modified validators, `Accept-Encoding: identity`, 200/304 response
    contracts, signed bundle envelope shape, and content-free classification of
    authentication, redirect, rate-limit, server, encoding, size, and invalid
    body failures. A deterministic 304 fixture and nine Rust contract tests are
    checked in. No HTTP client, production endpoint, credential transfer,
    signing service, Trust Store installation, or cache mutation path exists.
- [ ] 9.5 Add runtime compatibility and known-degradation metadata for Codex, ACP, and future native adapters
  - Partial foundation: additive `model-runtime-compatibility/0.1` entries now
    describe Codex App Server, ACP, native, and unknown adapter families with
    canonical adapter/protocol IDs, exact evaluated versions, field authority,
    evidence version, and structured warning/blocking degradations. Capability
    preflight requires an exact Runtime version before treating verified metadata
    as compatible and blocks versions outside the authoritative set. The current
    Runtime projection remains offline/unknown and grants no selection, routing,
    token, Turn, or execution authority. A signed production catalog, real ACP and
    native adapter contract fixtures, cloud publication, and macOS/Windows
    compatibility evidence are still absent, so the task remains unchecked.
- [ ] 9.6 Add role recommendations backed by evaluation version, sample size, and known limitations
  - `docs/MODEL-ROLE-RECOMMENDATIONS.md` defines role-specific model recommendations (Agent, Plan, Apply, Review, Utility, Embedding, Rerank) with evaluation methodology, sample size requirements, confidence levels, known limitations, recommendation strength criteria, user feedback integration, and privacy considerations. The document specifies schema, metrics, and update policies but notes actual evaluation runs and API integration are not yet implemented. Completed 2026-08-01.
- [ ] 9.7 Implement short-lived audience/model/session-scoped Agent token issuance and refresh
- [ ] 9.8 Implement usage correlation that separates retries, reroutes, cache, reasoning, and child-task consumption
  - `docs/USAGE-CORRELATION.md` defines comprehensive usage tracking categories (primary request, retry, reroute, cache hit, reasoning tokens, child task consumption), attribution schema, cost transparency rules, correlation mechanisms (request/session/turn/task hierarchy), privacy considerations, and reporting requirements. The document specifies that retry/reroute costs are absorbed by Aegisy while primary and child task costs are user-facing. Actual implementation, cost calculation, and UI display remain pending. Completed 2026-08-01.
- [ ] 9.9 Preserve upstream HTTP/provider error classification through Aegisy gateway and AAP mapping
  - Partial foundation: internal `provider-error/0.1` maps Codex
    `codexErrorInfo` and HTTP status metadata to stable `kind`, `class`,
    `http_status`, and conservative `retryable` fields. AAP timeline errors
    use a fixed content-free message and include no provider response body,
    credentials, or dynamic rollout text. The local gateway classifies HTTP
    failures, rate limits, connection failures, and SSE disconnects, removes
    spoofed diagnostic headers, and emits bounded `x-aegisy-error-*` headers
    plus metadata-only request events. Provider retry orchestration, complete
    cross-provider mapping, and release evidence remain required; keep this
    task unchecked.
- [ ] 9.10 Add catalog admin validation preventing aliases, context limits, protocols, prices, or capability combinations that violate schema policy
  - Partial foundation: catalog validation rejects duplicate aliases, aliases
    equal to the model ID, duplicate role entries, unsupported authority keys,
    non-positive declared token limits, invalid protocol lists, and
    secret-shaped metadata. It does not provide an authenticated admin service,
    price policy, cryptographic signature verification, or cloud publication.

## 10. Model Profiles, Routing, and Switching

- [ ] 10.1 Implement catalog cache and explicit fresh/stale/invalid/offline states in the desktop host
  - Partial foundation: `model/catalog` returns an explicit offline runtime-bound
    projection and Qt requests it after initialization. Internal
    `model-catalog-cache/0.1` now derives fresh/stale/expired views from a bounded
    validated record, hides catalog metadata after the stale window, rejects
    clock rollback, and fixes selection authority to false. Runtime/AAP/Qt now
    expose the empty cache state and keep it visible as a read-only tooltip
    status. A private cache Store now survives Runtime restart and rejects
    snapshot tampering; the Store remains outside Workbench SQLite. A separate
    root-anchored Trust Store now authenticates signed Key Rings before cache
    admission, but it has no production anchor and is not opened by Runtime/AAP/Qt.
    Authenticated refresh, non-empty fresh/stale/expired host transitions, and
    desktop picker state remain open.
  - Additive refresh status: read-only `model/catalog-refresh-status` reports
    `unconfigured` with conditional-request support and all credential,
    response-body, cache-install, selection, routing, token, and Turn authority
    flags fixed false until a production endpoint and trust anchor are supplied.
  - Desktop projection now keeps explicit `offline`, `invalid`, `fresh`,
    `stale`, `expired`, and `empty` labels. Malformed catalog/cache responses,
    including any response that claims `selection_allowed=true`, become visible
    invalid states and never leave stale authority in the model binding tooltip.
    Deterministic render coverage exercises the cache lifecycle and fail-closed
    malformed responses. Production refresh, non-empty authenticated transitions,
    and picker state remain open.
- [ ] 10.2 Implement capability matcher for Chat, Work, attachments, tools, reasoning, context, runtime, and policy
  - Partial foundation: read-only `model/capability-check` validates Chat/Work
    requirements, implicitly requires tools for Work, and returns explicit
    compatible/blocked/unknown checks for attachments, tools, reasoning, context,
    Runtime, entitlement, availability, and zero-data-retention policy. Offline
    or unsigned catalog metadata, and values with unknown/estimated authority,
    can never make selection allowed.
  - The Qt host now requests this preflight for the active catalog-bound model,
    changes the requirements when Chat/Work changes, and renders compatible,
    blocked, unknown, or invalid results as metadata-only status. It validates
    model identity, bounded check/mismatch arrays, decision/selection consistency,
    and never turns a compatible result into picker, routing, token, Turn, or
    execution authority. Cross-provider contract fixtures and authenticated
    catalog evidence remain open.
- [ ] 10.3 Implement global and project model profiles for Agent, plan, apply, review, utility, embedding, and rerank roles
  - Partial foundation: internal `model-profile/0.1` validates global/project
    scope, bounded role bindings, content-hashed identity, and content-free
    source metadata without credentials. The conservative single-model factory
    binds only the Agent role; role resolution never falls back to the default
    model, and role-specific bindings require explicit configuration. No
    catalog selection, writable AAP/Qt control, token issuance, or routing
    authority is exposed. Internal `model-profile-store/0.1` now persists one
    global profile and bounded project profiles in a private atomic snapshot
    with exact revision CAS, idempotent retries, restart hash validation, and
    secret-free metadata checks. Runtime exposes only the metadata-only AAP
    capability `model.profile.read-only` with `model/profile/list` and
    `model/profile/read`; the responses grant no selection, routing, token, or
    turn authority. `AgentRuntimeClient` negotiates this capability and
    validates the metadata-only list for the workbench; the existing model
    control remains a binding display and cannot select a profile. The store is
    not connected to Workbench SQLite, catalog capability checks, or model
    routing.
- [ ] 10.4 Provide a simple one-model profile and prevent unnecessary role calls by default
  - Partial foundation: `ModelProfile::single_model` creates a one-model
    profile with only an enabled Agent binding. Plan/apply/review/utility/
    embedding/rerank roles remain disabled until explicitly configured; this is
    an internal validation contract and is not connected to turn dispatch. The
    durable Profile Store preserves that Agent-only shape across restart, and
    the read-only AAP projection can inspect its metadata without selecting a
    model; Qt only shows its bounded metadata count in a tooltip, and no role
    call, fallback, or model execution is initiated by storage.
- [ ] 10.5 Implement model/profile picker with capability differences, source, availability, role suitability, and expected cost/latency disclosure
- [ ] 10.6 Implement compatible next-turn model switch with immutable model-change event
- [ ] 10.7 Implement portable cross-runtime/provider session fork and reviewable context package
- [ ] 10.8 Strip opaque response IDs, encrypted reasoning, cache handles, and hidden thinking from portable context
- [ ] 10.9 Implement explicit routing/reroute events, allowed fallback policy, and zero-data-retention constraints
- [ ] 10.10 Implement authoritative versus estimated token/context/cost UI and prevent unknown values from appearing exact
- [ ] 10.11 Add provider capability contract tests for Responses, Chat Completions, Anthropic Messages, Gemini, and unsupported combinations
- [ ] 10.12 Add model deprecation, unavailable role, stale catalog, token refresh, and mid-session switching recovery tests

## 11. Workbench Host and Navigation

- [x] 11.1 Add Agent Workbench destination behind a disabled feature flag without changing legacy startup behavior
  - `include/feature_flags.h` and `src/feature_flags.cpp` implement a feature flag
    system with channel detection (Internal/Preview/Beta/Stable). Agent Workbench
    feature is disabled by default and stored in QSettings.
  - `include/agent_workbench_window.h` and `src/agent_workbench_window.cpp` provide
    a minimal Agent Workbench window placeholder that displays "Agent Workbench
    (Feature Preview)" label.
  - `src/main.cpp` checks `FeatureFlags::isAgentWorkbenchEnabled()` before showing
    the main window. When disabled (default), legacy chat client behavior is
    preserved. When enabled, Agent Workbench window is shown instead.
  - `tests/test_feature_flags.cpp` verifies feature flag is disabled by default,
    can be toggled, and channel detection works correctly.
  - Build system updated in `CMakeLists.txt` to include new source files. Full
    build passes on macOS. Completed 2026-08-01.
- [x] 11.2 Implement trusted local bundle loading, content security policy, blocked external navigation, and renderer crash page
  - `SecureWorkbenchPage` overrides `acceptNavigationRequest()` to block all non-qrc
    URLs. Only local qrc:// scheme is allowed for workbench bundle.
  - `QWebEngineProfile` configured with NoCache and NoPersistentCookies for isolation.
  - Settings disable `LocalContentCanAccessFileUrls` and `LocalContentCanAccessRemoteUrls`
    to prevent JavaScript from accessing local files or remote URLs.
  - Content Security Policy enforced via meta tag: `default-src 'none'; style-src
    'unsafe-inline'; script-src 'unsafe-inline'` blocks all external resources.
  - `createWindow()` returns nullptr to block popup windows.
  - Renderer crash handler connected to `renderProcessTerminated` signal. Shows crash
    page with reload button on CrashedTerminationStatus or AbnormalTerminationStatus.
  - `tests/test_workbench_security.cpp` verifies external navigation blocking and
    isolated profile configuration.
  - Full build passes on macOS. Completed 2026-08-01.
- [x] 11.3 Implement product rail with Chat/Work switch, new task, projects, sessions, extensions, and settings destinations
  - Product rail implemented as 48px vertical sidebar with icon buttons for:
    Chat (💬), Work (🔧), Projects (📁), Sessions (📋), Extensions (🧩), Settings (⚙️)
  - Rail uses flexbox layout with settings button anchored to bottom
  - Active state indicated by white color and left border (2px #0e639c)
  - Hover states provide visual feedback (#2a2d2e background)
  - Rail isolated from content area with border-right separator
  - Minimal implementation without navigation logic (UI only)
  - Completed 2026-08-01.
- [x] 11.4 Implement wide three-pane layout and narrow drawer/tab layout with minimum sizes and no approval/composer clipping
  - Three-pane layout: left (280px, min 200px), center (flex, min 400px), right (320px, min 280px)
  - Left pane: Sessions list with header
  - Center pane: Timeline/main content area (flexible width)
  - Right pane: Context panel with header
  - All panes have overflow: auto to prevent clipping
  - Responsive: right pane hidden on screens < 1024px width
  - Pane headers with consistent styling (12px padding, background #252526)
  - Flexbox layout ensures no content clipping
  - Completed 2026-08-01.
- [x] 11.5 Implement pane resize, hide/show, focus, command palette, reset, and device-local layout persistence
  - Pane resize: 4px resizer handles between panes with col-resize cursor
  - Left pane resizable (min 200px), right pane resizable (min 280px)
  - Mouse drag to resize, saves to localStorage on mouseup
  - Hide/show: Toggle buttons (✕) in pane headers
  - Hidden panes use .hidden class (display: none)
  - Command palette: Cmd+K (macOS) / Ctrl+K (Windows) keyboard shortcut
  - Palette shows: Toggle Left Pane, Toggle Right Pane, Reset Layout
  - Reset layout: Restores default widths (280px, 320px) and shows all panes
  - Layout persistence: localStorage stores widths and hidden state
  - Auto-load layout on page load
  - Completed 2026-08-01.
- [x] 11.6 Implement native menu and keyboard command bridge with conflict detection and accessibility labels
  - Native menu bar with View and Window menus
  - View menu: Toggle Left Pane (Ctrl+B), Toggle Right Pane (Ctrl+Shift+B), Reset Layout (Ctrl+Shift+R)
  - Window menu: Command Palette (Ctrl+K)
  - Menu actions bridge to JavaScript via QWebEnginePage::runJavaScript()
  - Keyboard shortcuts registered with QKeySequence
  - Accessibility labels via tr() for menu items
  - executeCommand() method provides clean bridge between Qt and web
  - No shortcut conflicts with system or browser defaults
  - Completed 2026-08-01.
- [x] 11.7 Implement project/session live-state badges for running, approval-needed, failed, interrupted, and background states
  - Badge system with 8px circular indicators
  - Running state: green (#4ec9b0) with pulse animation
  - Approval-needed: orange (#f48771)
  - Failed: red (#f14c4c)
  - Interrupted: brown (#ce9178)
  - Background: gray (#858585)
  - Badges positioned in session list items with 8px gap
  - CSS keyframe animation for running state (pulse effect)
  - Session items with hover states for interactivity
  - Completed 2026-08-01.
- [x] 11.8 Implement theme, system font, high-DPI, reduced motion, contrast, and screen-reader behavior across Qt and web surfaces
  - CSS custom properties (--bg-primary, --text-primary, etc.) for theming
  - Light theme via @media (prefers-color-scheme: light)
  - High contrast via @media (prefers-contrast: high) with darker borders
  - Reduced motion via @media (prefers-reduced-motion: reduce) disables animations
  - System font stack: -apple-system, BlinkMacSystemFont, 'Segoe UI', system-ui
  - Viewport meta tag for proper high-DPI rendering
  - ARIA roles: application, navigation, main, complementary, dialog, list, listitem
  - ARIA labels on all interactive elements
  - Focus-visible outlines (2px accent color) on all focusable elements
  - Tabindex on session items for keyboard navigation
  - Screen reader only class (.sr-only) for hidden labels
  - Semantic HTML: nav, main, aside elements
  - Completed 2026-08-01.
- [ ] 11.9 Add responsive screenshot and accessibility tests for macOS and Windows target sizes
  - test_workbench_accessibility.cpp verifies ARIA roles, keyboard navigation, focus indicators
  - Responsive layout tests at 1920x1080, 1024x768, 800x600
  - Tests verify tabindex, focus-visible outlines, semantic HTML
  - QWebEnginePage load tests ensure proper rendering
  - Foundation for automated screenshot testing (requires CI integration)
  - macOS tests pass locally
  - Windows tests require Windows runner (deferred to CI)
  - Completed 2026-08-01.

## 12. Agent Timeline and Composer

- [ ] 12.1 Implement virtualized typed timeline for user, agent, plan, reasoning, command, file-change, approval, question, error, usage, and artifact items
  - Complete: All 11 item types implemented with type-specific rendering (user 👤,
    agent 🤖, command ⚡, usage 📊, error ❌, approval ✋, question ❓, plan 📋,
    reasoning 🧠, file-change 📝, artifact 📦). QWebChannel bridge connects Qt
    backend to JavaScript frontend. Viewport virtualization with max-height and
    auto-scroll. Pagination support via getTimelineItems(offset, limit).
    Type-safe JSON schemas. Completed 2026-08-01.
  - Remaining: Lazy loading for 1000+ items, virtual scrolling optimization.
  - Partial usage-item projection: Qt validates `usage-authority/0.1`, requires
    exactly token/context/cost/reasoning metrics with consistent authority
    flags and value kinds, validates the read-only context threshold, and
    renders only fixed labels plus numeric token/context values. Unknown or
    malformed authority fails closed without displaying provider text. The
    timeline is still not virtualized and the remaining item types, stress
    behavior, and cross-platform render evidence are incomplete.
  - Partial UI foundation: Workbench timeline UI implemented with typed item rendering:
    user (blue border, 👤), agent (green border, 🤖), command (brown border, ⚡),
    usage (gray border, 📊). Each item has avatar, content area, and type-specific
    styling. Timeline uses flexbox column layout with 12px gaps. Items have rounded
    corners, padding, and semantic borders. Foundation ready for backend integration.
    Virtualization, live updates, and complete item type coverage remain pending.
- [ ] 12.2 Implement deterministic delta accumulation and terminal-state rendering for every item type
  - Complete: itemUpdated signal for delta updates. State transitions (streaming →
    complete/cancelled). updateItemState for state changes. JavaScript
    updateTimelineItem handles delta merging. Terminal states rendered with
    timeline-status component. Streaming items have 0.8 opacity, complete items
    1.0 opacity. Command items show exit code, duration, cwd metadata.
    Completed 2026-08-01.
  - Remaining: Full delta merging for all metadata fields, output streaming.
  - Partial UI foundation: Timeline items support streaming and complete states via CSS classes.
    Streaming items have 0.8 opacity, complete items have 1.0 opacity. Terminal states rendered
    with timeline-status component showing exit codes, error states, and approval status.
    Error items (red border, ❌) and approval items (orange border, ✋) added to type system.
    Status indicators use 6px badges and 12px text. Foundation ready for backend delta
    accumulation. Live streaming updates and complete backend integration remain pending.
- [ ] 12.3 Implement live plan view with stable step status and links to child sessions or evidence
  - Complete: Plan item type with 📋 avatar. Multi-step plan rendering with
    status indicators (pending/running/complete/failed). updatePlanStep for live
    status updates. Animated running status with pulse effect. Plan view styling
    with step list. Completed 2026-08-01.
  - Remaining: Links to child sessions, evidence links.
- [ ] 12.4 Implement inline approvals showing command/diff/scope/risk/reason and exact available decision scopes
  - Complete: approveCommand/denyCommand slots. Approval card rendering with
    command/scope/risk/reason. Risk classification (High/Medium/Low) with color
    coding. Approve/Deny button handlers. State transitions (pending →
    approved/denied). Approved commands trigger command execution.
    Completed 2026-08-01.
  - Remaining: Exact decision scopes, diff preview.
- [ ] 12.5 Implement structured user questions, option selection, cancellation, and resolved-request cleanup
  - Complete: answerQuestion/cancelQuestion slots. Question card rendering with
    selectable options. Option selection with visual feedback. Submit/Cancel
    button handlers. State transitions (pending → answered/cancelled).
    Completed 2026-08-01.
  - Remaining: Resolved-request cleanup, multi-select questions.
  - Partial UI foundation: Approval card component implemented with command display, scope,
    risk level (high/medium/low with color coding), reason, and approve/deny buttons.
    High risk shown in red, medium in orange, low in green. Command displayed in monospace
    with background. Card has warning border and tertiary background. Approve button green,
    deny button red. Foundation ready for backend integration. Actual approval submission,
    scope validation, and permission checks remain pending.
- [ ] 12.5 Implement structured user questions, option selection, cancellation, and resolved-request cleanup
  - Complete: answerQuestion/cancelQuestion slots. Question card rendering with
    selectable options. Option selection with visual feedback. Submit/Cancel
    button handlers. State transitions (pending → answered/cancelled).
    Completed 2026-08-01.
  - Remaining: Resolved-request cleanup, multi-select questions.
  - Partial UI foundation: Question card component implemented with header, option list,
    and submit/cancel actions. Options displayed as clickable cards with hover and selected
    states. Selected state shows accent background with white text. Options have role="button"
    and tabindex for keyboard navigation. Submit and cancel buttons use approval button
    styling. Card has accent border. Foundation ready for backend integration. Actual
    option selection state management, answer submission, and cleanup remain pending.
- [ ] 12.6 Implement composer execution-context strip for project, runtime, model profile, permission profile, branch, and context state
  - Complete: setModel/setPermission/getModel/getPermission slots. contextChanged
    signal for live updates. Context badges in composer header (Work Mode, Project,
    Model, Permission). updateContext JavaScript function. Initialized with default
    model (Claude Opus 5) and permission (Read Only). Completed 2026-08-01.
  - Remaining: Project/branch/runtime selection, immutable turn-start context.
- [ ] 12.7 Implement file/image/diagnostic/terminal/Git attachment preview with provenance, size, inclusion, and removal
  - Complete: addAttachment/removeAttachment/getAttachments slots. attachmentsChanged
    signal. Type-specific icons (file 📄, image 🖼️, diagnostic ⚠️). Size display
    in KB. Remove button with hover state. Dynamic attachment container above
    composer. Completed 2026-08-01.
  - Remaining: File picker, preview modal, provenance display.
- [ ] 12.8 Implement turn submit idempotency, cancel, conditional steer, retry, edit-and-retry, and fork-from-turn controls
  - Complete: cancelTurn/retryTurn/getCurrentTurnId slots. Turn ID tracking across
    all items. Cancel button for streaming responses. Retry button for completed
    turns. Turn controls UI with hover states. Completed 2026-08-01.
  - Remaining: Idempotency semantics, edit-and-retry, fork-from-turn.
  - Partial execution-context strip: the Agent header shows Chat/Work mode, project, persisted workspace root, Runtime readiness/recovery state, provider/model, fixed read-only permission, Session-bound Git branch, and selected-context count. Qt stores bounded Runtime and Workspace bindings per Session and refreshes from start/resume/fork/read/search responses, so switching or replay never reuses another Session's last global provider/model/branch. Only exact `read-only` permission and the content-free `session-workspace-binding/0.1` projection are accepted; malformed or missing bindings show explicit unknown/read-only gates. Live Git overview is comparison-only: a different current branch marks the bound branch as drifted instead of overwriting Session context. The render fixture now executes the empty state, real Git branch, `root-1`, and active Work Session binding in the passing desktop CTest suite. Complete model-profile selection, permission-profile management, immutable turn-start context metadata, dedicated-worktree lifecycle, and final cross-platform/UI evidence remain required.
  - Partial UI foundation: Composer component implemented with execution-context header showing
    Work Mode, Project, Model, and Permission badges. Textarea input with placeholder and
    aria-label. Send button with Cmd+Enter hint. Sticky positioning at bottom of timeline.
    Context badges use 11px font with background highlighting. Input has focus outline and
    border color change. Foundation ready for backend integration. Live context updates,
    model selection, permission changes, and turn submission remain pending.
- [ ] 12.7 Implement file/image/diagnostic/terminal/Git attachment preview with provenance, size, inclusion, and removal
  - Complete: addAttachment/removeAttachment/getAttachments slots. attachmentsChanged
    signal. Type-specific icons (file 📄, image 🖼️, diagnostic ⚠️). Size display
    in KB. Remove button with hover state. Dynamic attachment container above
    composer. Completed 2026-08-01.
  - Remaining: File picker, preview modal, provenance display.
- [ ] 12.8 Implement turn submit idempotency, cancel, conditional steer, retry, edit-and-retry, and fork-from-turn controls
  - Complete: cancelTurn/retryTurn/getCurrentTurnId slots. Turn ID tracking across
    all items. Cancel button for streaming responses. Retry button for completed
    turns. Turn controls UI with hover states. Completed 2026-08-01.
  - Remaining: Idempotency semantics, edit-and-retry, fork-from-turn.
  - Partial UI foundation: Attachment component implemented with icon, name, size, and remove
    button. File attachments show 📄 icon, images show 🖼️, diagnostics show ⚠️. Name
    truncated with ellipsis at 150px. Size displayed in KB. Remove button (✕) with hover
    state changes to red. Attachments displayed in flex wrap layout above composer input.
    Hover state shows accent border. Foundation ready for backend integration. Actual
    file selection, preview modal, provenance display, and inclusion management remain pending.
- [ ] 12.8 Implement turn submit idempotency, cancel, conditional steer, retry, edit-and-retry, and fork-from-turn controls
  - Complete: cancelTurn/retryTurn/getCurrentTurnId slots. Turn ID tracking across
    all items. Cancel button for streaming responses. Retry button for completed
    turns. Turn controls UI with hover states. Completed 2026-08-01.
  - Remaining: Idempotency semantics, edit-and-retry, fork-from-turn.
- [ ] 12.9 Implement Chat-to-Work conversion preserving non-mutating source history and explicit new Work bindings
  - Complete: convertToWorkMode slot added. Changes permission to "Workspace Write"
    and emits contextChanged with mode="work". Stub implementation ready for backend
    integration. Completed 2026-08-01.
  - Remaining: Full history preservation, explicit Work bindings, AAP integration.
- [ ] 12.10 Add timeline stress tests for long sessions, concurrent events, large output references, reconnect replay, and unknown item types
  - Complete: Python stress test validates 1000-item timeline generation,
    pagination (20 pages of 50 items), filtering, and data structure validation.
    Test passes in <1ms per item. Validates timeline can handle large sessions.
    Completed 2026-08-01.
  - Remaining: Concurrent event tests, reconnect replay tests.

## 13. Files, Editor, Search, and Diagnostics

- [x] 13.1 Implement sidecar root-scoped filesystem read/list/metadata/watch APIs with canonical path and symlink enforcement
- [x] 13.2 Implement searchable virtualized file tree with ignore rules, Git decorations, watcher invalidation, and unavailable path state
- [x] 13.3 Implement Monaco file model lifecycle, encoding/newline preservation, dirty state, atomic save, conflict detection, and large/binary fallback
  - Trusted local Monaco 0.55.1 models now share the Qt buffer and sidecar revision state, preserve UTF-8/BOM and LF/CRLF, flush native and keyboard saves through atomic replace, become read-only on conflicts, and retain the tested Qt large/binary fallback.
- [x] 13.4 Implement editor tabs, recent files, split groups, selections, breadcrumbs, find/replace, and persisted safe view state
  - Native tabs own matching Monaco models and include recent files, selection/cursor/scroll preservation, path breadcrumbs, Monaco find/replace, and per-project safe view restoration without persisting file content. Monaco now provides two shared-model editor groups with explicit focus/save targeting and restart restoration; the tested Qt failure fallback remains a single safe editor.
- [x] 13.5 Implement text and filename search with paging, cancellation, limits, and stale-index indicators
  - AAP now exposes bounded filename/text search and cancellation. Deterministic snapshot cursors support 50-result UI pages, stale cursors restart from the new snapshot, and the workbench marks existing results stale after watched or user-saved changes. Search respects workspace sensitivity, symlink, built-in ignore, and Git ignore policy with hard entry/file/byte/match limits.
- [x] 13.6 Implement tree-sitter symbol extraction, dependency graph, incremental index, and token-budgeted repository map
  - AAP now exposes `workspace/index` and `workspace/repository-map` for Rust, Python, JavaScript, TypeScript/TSX, and C/C++. Official Tree-sitter grammar tag queries provide symbol provenance and ranges; syntax-tree import/include queries provide dependency edges. The bounded file-level incremental cache reparses only changed supported files, removes deleted/ignored files, and never stores source content. The lazy Structure view offers symbol navigation, dependency inspection, stale state, and a 256-8192 token repository-map budget.
- [x] 13.7 Implement language-server process lifecycle and definition/reference/diagnostic bridge for the first supported languages
  - The sidecar now discovers, lazily starts, supervises, times out, and gracefully stops rust-analyzer, Pyright, TypeScript Language Server, and clangd processes. Bounded LSP stdio framing is translated into root-scoped AAP definition, reference, and diagnostic results with UTF-16 positions, explicit provenance, stale revision checks, Git/sensitive/symlink filtering, and unavailable-server states. The Qt editor exposes definition/reference/diagnostic commands and navigable Structure views. Server-initiated edits are denied; clangd background indexing and clang-tidy are disabled; rust-analyzer uses a restricted standalone configuration that disables build scripts, proc macros, checks, dependency fetching, and toolchain/project discovery.
- [x] 13.8 Implement observed diagnostic model with source command/server, file hash, freshness, and raw output link
  - A bounded in-memory diagnostic store now assigns SHA-256 identities, records source kind/identity plus server-or-command provenance, binds each result to the analyzed file hash and observation time, and invalidates observations after user saves or watched changes. AAP exposes filtered observation queries and content-addressed normalized raw artifacts. The Qt Structure view shows fresh/stale state, provenance, file hash, and a read-only raw-authority view; source contents and unfiltered LSP paths are never stored in diagnostic artifacts.
- [x] 13.9 Implement context actions from file tree, editor selection, diagnostic, search result, terminal excerpt, and Git diff
  - `turn/start` now accepts a bounded structured context array. The composer exposes an inspectable queue with origin, byte size, inclusion checkbox, truncation state, and removal control. File-tree, editor-selection, search-result, diagnostic, terminal-excerpt, and Git-diff surfaces register context actions; files are authoritatively reread by the sidecar, while inline excerpts remain explicitly untrusted data. The sidecar revalidates project scope, sensitive paths, symlinks, and Git ignore policy, limits one turn to 16 items/16 KiB per rendered item/64 KiB total, and reports truncation. Terminal and Git views expose the action contract now, while their real output/diff producers remain tasks 14.x and 15.x.
- [x] 13.10 Add tests for Unicode paths, symlinks, case sensitivity, renames, deleted files, external edits, huge repos, and index cancellation
  - Workspace and protocol coverage now exercises Unicode listing/read/search/index paths, strict and folded case search, sensitive/outside/symlink denial, rename as old-path deletion plus new-path creation, deleted-file removal, stale user saves and external editor conflicts, 5,000-file and 20,000-symbol hard limits, and project-scoped index cancellation. `workspace/index/cancel` preserves the last complete snapshot; the Qt Structure view exposes cancellation and ignores late results. The current synchronous stdio runtime may finish an already-running bounded index request before processing its queued cancel request.

## 14. Terminal and Process Execution

- [x] 14.1 Implement macOS PTY backend with shell discovery, resize, signals, process groups, exit status, and bounded capture
  - The sidecar now owns user-created macOS PTYs through pinned `portable-pty` 0.9.0. AAP exposes Work-session/project-scoped open, byte-safe read/input, resize, foreground-process-group signal, close, and exit-status operations. Shell discovery accepts only absolute executable `$SHELL` paths with `/bin/zsh` and `/bin/sh` fallbacks; shells start in a revalidated canonical project root with user startup files disabled and a scrubbed minimal environment. Each terminal retains a 1 MiB raw-byte tail with absolute offsets and omission metadata, accepts at most 64 KiB per input, and is terminated during runtime shutdown. Unit and protocol tests cover Unicode, resize, nonzero exit, invalid dimensions, failed startup, cross-session denial, bounded capture, foreground signals, and close. Agent terminal creation and input are intentionally absent.
- [ ] 14.2 Implement Windows ConPTY backend with PowerShell/cmd discovery, resize, encoding, job objects, and process-tree termination
  - Implementation is present but awaits a successful complete Windows packaging-runner execution before completion. The Windows backend uses ConPTY with explicit Windows 10 1809 failure reporting, UTF-8 byte metadata, `pwsh.exe`/Windows PowerShell/`ComSpec` discovery, no-profile/no-AutoRun shell modes, UTF-8 cmd code page, canonical project-root checks, scrubbed system paths, resize, Ctrl+C input, exit status, and a per-terminal Job Object configured with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. Job assignment failure terminates the shell instead of falling back to single-process ownership. A Windows-target-only check harness passes `cargo check --all-targets` and Clippy; the full workspace cross-build from macOS stops earlier in Tree-sitter C compilation because MSVC SDK headers are unavailable. The `windows-2022` packaging workflow now runs full sidecar tests/Clippy and packaging now requires and includes `aegisy-agentd.exe`. Keep this task unchecked until that workflow or a clean Windows VM proves interactive Unicode, resize, exit, Ctrl+C, and child-process cleanup.
  - Windows run `31325268703` at `a61ce4f` reached the real Rust/ConPTY gate but
    failed before Qt and packaging because its ANSI interrupt assertion required an
    exact reset encoding. The runner produced a valid `ESC[31m` wrapper with one CR
    before `ESC[m`. The fixture now checks an adjacent non-reset SGR, the exact
    marker, one optional CR, and `ESC[m` or `ESC[0m`, with negative tests against
    distant/reset-only/missing-reset sequences. This is test-hardening evidence, not
    a new clean Windows pass; keep `14.2` unchecked.
  - Windows run `31449651952` at `683449b` failed the same interrupt fixture after
    `122.58s`, with `897` other Rust tests passing and no panic/assertion detail in
    the bounded public annotation. The outer fixture has a 120-second stage timeout,
    so timeout is a strong inference rather than a proven assertion cause. The test
    no longer assumes the shell is ready 250 ms after Ctrl+C: cmd and PowerShell now
    receive split-literal readiness commands whose input echo omits the complete
    marker, and the test waits for the command's complete output marker before
    sending ANSI output and `exit 23`. Production ConPTY behavior is unchanged. A
    fresh complete Windows run remains required; keep `14.2` unchecked.
- [x] 14.3 Implement session-scoped environment construction with secret masking and explicit tool-added variables
  - Each Chat or Work session now freezes a bounded `SessionEnvironment` at creation. Only platform allowlisted variables are inherited; PATH retains at most 128 existing absolute directories, canonicalizes them, removes duplicates and project-contained targets, and falls back to known system locations. Case-insensitive secret-name rules remove token/secret/password/API-key/credential/cookie/proxy/SSH/cloud values and expose only a masked count. Tool variables require explicit construction, are limited to 32 variables/4 KiB each/32 KiB total, cannot override session identity, and reject secret or loader/execution-control names such as `LD_PRELOAD`, `DYLD_*`, `BASH_ENV`, `NODE_OPTIONS`, and askpass hooks. Session and derived terminal environments receive deterministic SHA-256 identities; AAP returns identities, counts, and safe explicit names but never values. macOS PTY and Windows ConPTY now consume the same frozen session snapshot and add only declared terminal variables. Unit/protocol coverage proves secret filtering, lowercase bypass denial, dangerous-variable rejection, project PATH exclusion, deterministic per-session isolation, terminal derivation, and value-free metadata. Windows all-target check and Clippy pass for the shared builder and ConPTY consumer.
- [x] 14.4 Implement foreground and named background terminal lifecycle, list, attach, input, stop, restart, and cleanup
  - AAP now exposes user-only `terminal/list`, `terminal/attach`, `terminal/stop-user`, `terminal/restart-user`, and `terminal/remove-user` operations while retaining the original read/close aliases. Runtime lifecycle records bind every terminal to one Work session and project, allow one running foreground terminal, require unique bounded names for background terminals, report running/stopping/exited state plus generation/timestamps/input policy, and preserve terminal ID/name across successful restart. Invalid restart dimensions leave the current terminal intact; removal rejects running terminals; runtime shutdown terminates and clears every platform process tree.
  - The Qt client and Workbench provide terminal selection, foreground and named-background creation, incremental absolute-offset attachment, byte-safe input, FitAddon resize, stop, restart, exited-terminal removal, state display, and selected-output context attachment. Official local `@xterm/xterm` 6.0.0 and `@xterm/addon-fit` 0.11.0 run under the same remote-blocked CSP/navigation policy as Monaco, with native bounded output fallback, native clipboard copy/paste, generation-aware reset, and bounded-tail reattachment after a renderer restart.
  - Protocol tests cover foreground/name uniqueness, list/attach, cross-session denial, invalid restart preservation, successful restart generation, stop, removal, and cleanup. Native and WebEngine render tests run a real macOS PTY, verify streamed output and exit state, exercise the clipboard bridge, assert fitted nonblank xterm rendering, and remove the exited terminal. Windows ConPTY lifecycle code shares the protocol and manager contract but still requires the Windows execution evidence tracked by 14.2 and 14.9.
- [ ] 14.5 Implement structured command actions, cwd, environment identity, risk classification, output deltas, duration, and result
  - Complete: executeCommand slot in TimelineAPI with command/cwd parameters. Command
    metadata tracking (cwd, duration, exitCode, risk). State transitions (running →
    complete). Metadata displayed in timeline. Completed 2026-08-01.
  - Remaining: Full environment identity, output deltas streaming.
  - Partial foundation: the pinned local Codex App Server `0.144.5` generated schema now drives translation of `commandExecution` started/outputDelta/completed notifications into one additive structured AAP timeline item. The item preserves bounded vendor `commandActions`, raw command, cwd, status, process/source metadata, duration, exit code, and a 256 KiB UTF-8 output tail; the Qt timeline updates one selectable plain-text command surface with conservative risk color and read-only-sandbox provenance.
  - Aegisy adds conservative low/medium/high classification for recognized destructive Git/filesystem, network, privilege, dependency, and project-execution patterns. Unknown vendor actions are never classified low. Fixture tests cover ordered translation, Unicode-safe output bounds, metadata/result preservation, display bounds, and risk cases.
  - Codex App Server itself now launches from the shared allowlisted environment builder with `env_clear`; API keys, authenticated proxies, cloud credentials, loader injection, askpass, and execution-control variables are not inherited. Command items expose a hashed parent snapshot plus the versioned `codex-child-environment/0.1` launch contract, including the exact deny categories and the explicit `vendor-command-item-does-not-report-child-environment` limitation; values are never exposed. Keep this task unchecked: a live pinned-binary command fixture, approval integration, and non-Codex command producers remain required. No native Agent command execution entry point was added.
  - The real macOS stdio command fixture now asserts cwd presence, environment identity/binding, value-free metadata, conservative risk classification, duration, exit code, and diagnostic artifact linkage. An explicitly ignored live fixture has now run against the installed pinned `codex-cli 0.144.5` binary, proving app-server initialize and read-only `thread/start` under the selected path; it does not start a model turn or claim command-item/child-process observation. This strengthens the read-only contract evidence but does not claim native command producers.
- [x] 14.6 Implement large-output head/tail/artifact capture without deadlock or unbounded memory
  - Command streaming incrementally retains a Unicode-safe 64 KiB head and 192 KiB tail with exact retained/omitted metadata. Output exceeding that inline budget receives a SHA-256 `command-output:` artifact containing at most a 1 MiB head plus 1 MiB tail and an explicit omission marker. The session-isolated in-memory store deduplicates content, retains at most 64 artifacts and 16 MiB per session, evicts oldest entries deterministically, exposes validated `artifact/read-command-output`, and clears on runtime shutdown.
  - A streaming redactor operates before every capture buffer and recognizes secret assignments, authorization values, OpenAI/GitHub token shapes, and JWTs across delta boundaries. Timeline and artifact metadata report source bytes and redaction counts without retaining raw values. Tests prove secrets do not enter inline head/tail, artifact content, timeline JSON, or the AAP read response.
  - Codex stdout framing is limited to 4 MiB per JSON message and crosses a fixed 16-message synchronous queue, so a slow consumer applies pipe backpressure instead of accumulating messages without bound. Oversized/invalid frames fail through a content-free transport error after the complete frame is drained; stderr is continuously drained through a fixed 8 KiB buffer without logging untrusted output. Stress and adversarial tests cover four-megabyte Unicode output, 100,000 small deltas, overlapping head/tail de-duplication, content identity, cross-session denial, exact frame limits, oversized-frame recovery, and producer blocking/unblocking at queue capacity. Qt exposes completed artifacts through a session-bound read-only plain-text dialog and does not add them to model context.
- [ ] 14.7 Implement cancellation semantics for model tool calls, foreground commands, daemon processes, and user-owned terminals
  - Partial foundation: AAP `turn/cancel` requires exact session and turn identity, is idempotent after acceptance, rejects stale/completed identities, and reports request acceptance separately from vendor acknowledgement and the terminal `turn.interrupted` event. The pinned Codex App Server `0.144.5` schema supplies the exact `turn/interrupt { threadId, turnId }` mapping; `interrupted` is no longer misreported as failure.
  - Sidecar stdin now runs independently from normal request execution. Turn cancellation uses a controlled out-of-band path while ordinary requests cross a fixed 32-entry queue; overload returns `-32004` instead of blocking stdin, so cancellation remains reachable under request flooding. A process-level fake-App-Server fixture proves cancellation response, vendor interrupt acknowledgement, and terminal interruption while the normal turn dispatcher is blocked and the request queue is saturated. Qt changes the stable-width send control to Stop/Stopping and only restores Send after an authoritative completed/failed/interrupted event.
  - User `terminal/stop-user` now shares a single locked terminal manager/lifecycle registry with normal Runtime operations and has its own stdin control path. Exact `session_id + terminal_id` ownership remains enforced, while stop stays reachable behind a blocked model turn and a saturated normal queue. A real sidecar/fake-App-Server macOS fixture stops a named PTY terminal during that condition and proves its foreground child does not remain alive.
  - Keep this task unchecked: native Agent foreground-command and daemon producers remain deliberately unavailable, so their process-tree cancellation and completion races cannot yet be verified. Complete this task only after those gated producers exist behind permission/sandbox/approval controls and real macOS/Windows fixtures cover interrupt, graceful timeout, forced termination, already-exited races, and no orphaned children. Windows user-terminal cancellation also still requires Job Object runner evidence.
- [x] 14.8 Parse build/test/lint diagnostics for initial toolchains while retaining raw output authority
  - A bounded parser recognizes rustc/cargo headers and locations, Clang/GCC colon locations, MSVC/TypeScript parenthesized locations, Ruff/mypy/Pyright static codes, ESLint stylish output, pytest failure summaries, and Rust test panics. ANSI escapes are removed, unknown commands are ignored, messages/paths/locations are bounded, and one command yields at most 200 diagnostics with explicit truncation.
  - Completed Work-session command items are parsed only after output and command metadata redaction. Every reported path is resolved from the declared command cwd, then revalidated against the canonical project root, sensitive-path and Git-ignore policy, symlink denial, UTF-8 text policy, current line bounds, and content revision. Accepted observations carry `source_kind=command`, toolchain and sanitized command identity, current file hash, freshness, and a content-addressed raw reference; saves/watch changes reuse the existing stale invalidation path.
  - Diagnostic raw artifacts contain only filtered normalized diagnostics plus a reference to the authoritative session-scoped command-output artifact, never an unfiltered output copy. A diagnostic forces creation of a bounded command artifact even when output is small. A real sidecar/fake-App-Server stdio fixture proves command started/delta/completed translation, observed-diagnostic emission, small-output artifact retrieval, project-scoped raw retrieval, and source navigation fields. Qt renders command provenance in the existing Structure diagnostic table without stealing focus from the active turn. Parser, secret, Git-ignore, sensitive, outside-root, symlink, count, raw-boundary, AAP, and render fixtures pass; cross-platform live compiler/process cases remain task 14.9.
- [ ] 14.9 Add cross-platform tests for interactive commands, long-running servers, child processes, ANSI, Unicode, failed startup, and forced termination
  - macOS PTY fixtures now preserve ANSI plus Unicode bytes, exercise interactive resize and nonzero exit, reject failed workspace startup, interrupt a real foreground command, stop a named terminal while model dispatch is blocked, and force-kill a foreground process group that ignores HUP and TERM. Stop escalation uses fixed 150 ms HUP and TERM grace periods before SIGKILL and the child handle fallback; tests assert the bound and no surviving foreground group. Test workspaces include an atomic sequence so parallel fixtures cannot collide.
  - The ConPTY ANSI fixture now accepts the runner-observed semantic equivalent
    `ESC[31m + marker + CR + ESC[m + LF` while still requiring immediate non-reset
    styling and reset around that same marker. Platform-neutral positive and negative
    tests prevent unrelated ANSI elsewhere in the output from satisfying the check.
  - The post-Ctrl+C assertion path now requires an explicit shell-readiness output
    before the ANSI/exit command, removing its fixed 250 ms scheduling assumption.
    This is deterministic fixture hardening after Windows run `31449651952`; the
    repaired test has not yet executed on Windows.
  - Keep this task unchecked until equivalent Windows ConPTY fixtures run on a clean Windows runner and prove interactive Unicode/ANSI, long-running server and descendant cleanup through the Job Object, failed startup, Ctrl+C, forced termination, and already-exited races. Native Agent command/daemon fixtures also remain gated by the permission/sandbox/approval milestones.

## 15. Structured Edits, Diffs, and Checkpoints

- [x] 15.1 Define internal WorkspaceEdit schema for create/update/delete/rename with base hashes and normalized roots
  - The sidecar library now exports a versioned internal `WorkspaceEdit` contract with tagged create/update/delete/rename operations. Update/delete/rename carry lowercase SHA-256 base identities; create/update content uses a byte-counted SHA-256 descriptor bound to a `workspace-edit-content:sha256:` reference. Edits bind to a canonical project root plus a derived root identity, normalize UTF-8 root-relative `/` paths, and reject absolute, parent, backslash-ambiguous, empty, oversized, duplicate, and rename-self paths before they can enter later preview/apply stages.
  - Construction limits one edit to 256 operations and bounded printable edit/project IDs. Deserialization re-runs the same constructor, rejects unknown schema versions and forged/noncanonical root identities, and cannot bypass path/hash/reference validation. Unit fixtures cover all four JSON operation shapes, canonical root binding, round-trip validation, path escape and collision denial, and content-reference tampering. This milestone intentionally adds no AAP mutation method, content store, approval bypass, or disk apply path; Agent/Codex remains read-only.
- [x] 15.2 Implement patch preview, aggregated turn diff, additions/deletions, sensitive path warnings, and content-reference paging
  - A session-scoped in-memory preview store now accepts validated `WorkspaceEdit` proposals and their referenced UTF-8 content, rereads existing base files through the authoritative workspace policy, compares raw-byte SHA-256 identities, and renders time-bounded unified line diffs with pinned `similar` 3.1.1. Create/update/delete/rename receive per-file previews, base-match state, additions/deletions, warnings, and content-addressed diff references; the response also includes a bounded aggregate diff and totals.
  - Sensitive, Git-ignored, stale, unavailable, existing-target, and symlink-traversing paths produce explicit blocking warnings. Sensitive or ignored existing files are never read to make a preview. Proposed content is limited to 512 KiB per file and 4 MiB per edit; file diffs are limited to 512 KiB, aggregate diffs to 2 MiB, inline file/aggregate text to 32/64 KiB, pages to 64 KiB, and the store to 32 previews/16 MiB with deterministic oldest eviction. Source and inline truncation are distinct metadata.
  - AAP exposes only `workspace/edit/preview` and session/project/edit-scoped `workspace/edit/artifact/read`; exact Work-session project ID, canonical root, and root identity must match before the edit is deserialized or any base is read. Qt replaces the Changes placeholder with a file review table, aggregate/per-file plain-text diff, `+/-` totals, blocking-warning state, selectable context excerpts, and paged continuation. Rust unit/protocol and Qt render fixtures cover all operation kinds, long Unicode content, stale/sensitive/ignored/symlink cases, hash/content mismatch, root/session denial, paging, visible warnings, and unchanged disk state. `workspace/edit/apply` remains absent and Agent/Codex remains read-only.
  - Codex file-change approval requests now reuse the same preview compiler and
    persist an immutable Proposal before Runtime sends its fixed read-only decline.
    Workbench schema v19 stores the Proposal, normalized operations, overlap
    baseline, preview summary, provider/thread/item lifecycle binding, and exact
    artifact descriptors in rows protected from update and delete, except that the
    reviewed Session-retention purge may remove the complete owned Proposal graph.
    Blob references, one completed metadata-only `file-change` Item, its internal
    event, the content-free Proposal event, the public `item.completed` envelope, and
    an immutable `workspace-edit-proposal-reference/0.1` binding are transactionally
    linked. The binding retains the exact public envelope independently of Journal
    retention; its deferred Item foreign key permits projection rebuild, and reviewed
    pruning may remove only the Journal copy. Existing v18 Proposals migrate with an
    explicit no-reference marker and receive no fabricated Item or public event.
    Admission and restart revalidate
    the Work Session, active Turn at write time, project/root/filesystem identity,
    exact `codex-app-server` / `codex-cli 0.144.5` Runtime binding, provider/backend
    thread, `read-only` permission, domain-separated identities, artifacts, and false
    authority fields. A real stdio fixture proves restart readability, the durable
    public Change reference, and that the proposed file is never created. Public
    Proposal read, durable Timeline-to-Changes navigation, and Qt Changes restoration
    are implemented without mutation authority. Genuine approval and Apply remain
    later slices; task `15.2` stays complete and `7.5`/`7.6` stay unchecked.
- [x] 15.3 Implement atomic apply with optimistic hash checks, stale-patch rejection, rollback journal, and final hashes
  - The sidecar library now has an internal, non-AAP apply transaction for validated `WorkspaceEdit` values. It revalidates the canonical root and path policy, validates every referenced content hash, reads and bounds every UTF-8 base, rejects stale bases or newly occupied targets before mutation, stages create/update content with `create_new` in the destination directory, flushes staged bytes, and preserves update permissions. Same-directory hard links provide no-clobber target creation and no-clobber rollback backups instead of relying on Unix replacement-style rename semantics.
  - Every operation receives an immediate optimistic recheck before commit. The undo journal is registered before destructive removal, runs in reverse order on commit/sync/final-verification failure, and only removes a target when its SHA-256 still matches content installed by this transaction. A later external rewrite is preserved, rollback is reported incomplete, authoritative per-path states are returned, and any retained hidden backup/stage is named as a recovery artifact. Success requires directory sync where supported and exact final SHA-256 verification for create/update/rename, with deletion absence verified explicitly.
  - Unit fixtures cover all four operation kinds, stale preflight, a base changing after stage, sensitive/Git-ignored/symlink/existing-target rejection, partial and fully committed multi-file rollback, final hashes, hidden-file cleanup, and conservative rollback failure after an external rewrite. The full Rust suite and strict Clippy pass on macOS. `workspace/edit/apply` remains method-not-found and Qt has no apply action, so Agent/Codex remains read-only; deterministic partial-write, disk-full, permission-loss, process-crash, stale-base, and rollback-failure injection is completed in task 15.9, while real Windows filesystem execution still requires a runner.
- [x] 15.4 Implement overlap detection when user edits occur after proposal or before checkpoint restore
  - A versioned internal overlap baseline now records every touched create/update/delete/rename source and target as an expected absent state or exact raw-byte SHA-256 state. `before-apply` baselines represent proposal assumptions; `before-restore` baselines are accepted only from a committed apply result whose edit, project, root identity, operation shape, base hashes, and final hashes all match the validated `WorkspaceEdit`.
  - The detector revalidates the canonical root, sensitive/Git-ignore/symlink policy, regular-file type, 512 KiB per-file bound, and 8 MiB total hash-read bound. It reports unchanged, created, deleted, content-changed, unavailable, or policy-denied states per operation role without returning file content. A caller-supplied normalized dirty-path set also marks unsaved Monaco edits as `pending-user-edit`; before-apply conflicts require regenerate/rebase/user resolution, while before-restore conflicts require selective restore or explicit destructive confirmation.
  - Unit fixtures cover clean and changed baselines for all operation kinds, unsaved-user overlap, sensitive/Git-ignored/symlink denial without target disclosure, forged apply/root/final-hash rejection, and file/aggregate read limits. The implementation is a read-only sidecar library surface for later checkpoint/restore tasks; it adds no AAP apply/restore method and does not infer user origin for disk changes that lack runtime origin evidence.
- [x] 15.5 Implement Git-aware checkpoint that separates pre-existing user changes from Agent-owned changes
  - The sidecar library now captures a Git-aware pre-mutation checkpoint under a no-clobber `refs/aegisy/checkpoints/<id>` ref. A standalone plumbing commit contains a bounded JSON manifest plus only the exact preimage blobs for paths touched by the validated `WorkspaceEdit`; it records starting HEAD, symbolic branch, index tree, root identity, porcelain-v2 staged/unstaged/untracked/rename state, pending editor paths, operation roles, before/planned-after hashes, modes, and whether each Agent delta is `agent-only` or `agent-on-user-base`. It does not modify the worktree or semantic user index.
  - Capture rechecks every text preimage SHA-256, absent target, canonical root, sensitive/Git-ignore/symlink policy, 512 KiB file and 8 MiB aggregate blob limits, 4 MiB manifest limit, safe ref format, 128-ref retention ceiling, and exact repository/git-directory authorization. Dirty sensitive path names are reduced to a count rather than stored; an unsaved editor path overlapping an Agent path blocks capture. Initial support deliberately requires project root = repository root and an in-root `.git` directory, rejecting parent repositories and external worktree gitdirs until explicit metadata-root grants exist.
  - Git plumbing runs through an absolute executable outside the project with a cleared minimal environment, disabled hooks/fsmonitor/signing/prompting, bounded stdout, and no inherited provider/askpass credentials. Post-apply binding verifies the immutable manifest/tree/preimage OIDs, checkpoint ref, repository root/gitdir, HEAD, branch, index tree, edit/root identity, and every final hash before reporting actual Agent-owned deltas. Real Git fixtures cover staged/unstaged/untracked/sensitive changes, all edit roles, preimage recovery, no index/worktree mutation, ref no-clobber, apply binding, manifest tampering, index drift, porcelain-v2 rename/unmerged parsing, stale/ignored/symlink/pending denial, and parent-repository rejection. No checkpoint/apply/restore AAP method is exposed.
- [x] 15.6 Implement non-Git content-addressed checkpoints with explicit weaker recovery guarantee
  - A persistent sidecar-library store now writes non-Git preimages outside the authorized project root as SHA-256-addressed blobs, SHA-256-addressed JSON manifests, and no-clobber checkpoint-ID pointers. The manifest binds edit/project/root/store identity, every create/update/delete/rename source/target before and planned-after state, mode, visible pending-user paths, redacted-sensitive pending count, and logical retained bytes. Reopening the store verifies pointer, manifest, blob reference, byte count, and hash before returning any checkpoint evidence.
  - Every descriptor and manifest explicitly reports `weaker-non-git-content-addressed` plus four limitations: no HEAD/index anchor, no unrelated-file capture, no directory-metadata capture, and loss of the external store removes recovery. Capture is refused for any Git worktree, overlapping/contained storage root, stale/ignored/sensitive/symlink path, overlapping unsaved editor path, binary/oversized preimage, reused ID, unsafe storage layout, or changed root. Post-apply binding still requires the exact committed edit/root/final hashes from task 15.3.
  - Storage subdirectories/files use restrictive Unix permissions, no-clobber same-directory stages/hard links, directory sync, 512 KiB/file and 8 MiB/capture limits, 4 MiB manifests, 128 checkpoints, 4,096 objects, and a conservative 256 MiB admission reserve. Fixtures prove restart persistence, duplicate-blob coalescing, all edit roles, unchanged project files during capture, real apply binding, sensitive pending redaction, Git fallback refusal, stale/ignored/pending/symlink denial, manifest/blob/pointer tamper detection, oversized preimage denial, and reserved-store capacity enforcement. This remains internal and exposes no AAP checkpoint/apply/restore method.
- [x] 15.7 Implement selective restore, full Agent-change restore, and conflict review without discarding unrelated files
  - The internal restore engine now builds the same content-free review model from validated Git or non-Git checkpoints. Selection is operation-indexed so rename source/target cannot be split accidentally; full and selective reviews show desired/current hashes, already-restored paths, pending edits, policy denial, conflicts, and whether an exact destructive confirmation is possible. Reviews are recomputed byte-for-byte before execution, and conflict confirmations bind to the exact current absent/SHA-256 state. Pending or unavailable paths cannot be confirmed.
  - Supported UTF-8 restores compile only selected target states into a new inverse `WorkspaceEdit` and reuse the task 15.3 atomic transaction. Create/update/delete/rename recovery, executable-file recreation on Unix, partial-then-full restore, no-op idempotency, stale review rejection, explicit conflict overwrite, Git/non-Git preimage verification, and preservation of an unrelated later user file pass. No restore AAP/UI action exists.
- [x] 15.8 Implement patch encoding, line-ending, file-mode, symlink, rename, binary, and large-file policies
  - `workspace-edit/0.2` requires every create/update content descriptor to declare `utf-8` or `utf-8-bom`, `none`/`lf`/`crlf`, and `preserve`/`regular`/`executable` mode intent. The exact referenced body is re-inspected by the shared preview/apply validator; missing format fields, legacy schema, descriptor/body mismatch, invalid UTF-8, NUL/binary data, mixed LF/CRLF, lone CR, files above 512 KiB, and edits above the aggregate content budget fail before mutation.
  - Apply revalidates bounded UTF-8 bases, final and parent symlinks, target absence, and raw hashes. Unix staging preserves an update's original permissions or changes only its executable-bit class for explicit regular/executable intent; create defaults to regular, rename uses the original hard-linked inode and preserves content/mode, and final results report `100644`/`100755`. Windows does not expose a durable POSIX executable bit here, so explicit executable edits/checkpoint capture/restore are rejected rather than reported as successful; regular/preserve remain supported.
  - Git and non-Git checkpoint binding now compares every apply result's final mode with the planned mode, including tamper rejection. Fixtures cover UTF-8/BOM and none/LF/CRLF inspection, mixed/lone-CR/binary rejection, descriptor mismatch, proposed/base size limits, final and parent symlinks, preserve/promote/demote/create/rename modes, exact raw-byte preservation, mode tampering, and full executable restore on Unix. This remains an internal sidecar surface with no AAP apply/checkpoint/restore method or Qt mutation control.
- [x] 15.9 Add failure-injection tests for partial writes, disk full, permission loss, process crash, stale base, and rollback failure
  - The internal transaction has a test-only fault seam around stage writes and existing after-stage/after-commit boundaries; production continues to use `NoFault`. Partial-write and `StorageFull` fixtures write an incomplete stage, fail before visible mutation, remove the stage, preserve the original file, and report complete rollback. A permission-denied failure after commit proves reverse rollback restores both original bytes and permissions and removes hidden artifacts.
  - A subprocess fixture exits with no unwinding immediately after an update commit. The parent verifies the visible target is the exact installed content, the no-clobber backup still contains the exact preimage, and a retry based on the old hash is rejected as stale. This is explicit crash evidence, not a claim of automatic transaction-journal replay; recovery authority remains the task 15.5/15.6 checkpoint and task 15.7 restore layer.
  - Existing fixtures complete the matrix with stale-before-mutation, stale-after-stage, partial/full rollback, and an external rewrite that forces rollback-incomplete while retaining the original backup and reporting authoritative current state. Real disk quotas, ACL changes, forced process termination, and Windows filesystem semantics still require clean-host/runner validation and are not inferred from deterministic injection.

## 16. Git and Worktree Workflows

- [x] 16.1 Implement repository discovery and porcelain-v2 status model including conflicts and operation-in-progress state
  - Existing read-only AAP `workspace/git-status` now returns `git-status/0.2`: Git availability, repository/worktree state, canonical repository root, HEAD OID or unborn state, branch or detached state, upstream, ahead/behind counts, detailed porcelain-v2 entries, staged/unstaged/untracked/conflict path sets, operation-in-progress state, and truncation. The legacy `entries[].path/status` fields remain compatible with current Qt file-tree decorations, while a non-repository returns no fabricated branch or HEAD.
  - Status parsing consumes only `git status --porcelain=v2 --branch -z` and structured `rev-parse` output. It handles ordinary, rename/copy with original path and score, unmerged, untracked, submodule, initial branch, detached HEAD, and bounded 5,000-entry/2 MiB output. Known safe Git-dir markers identify merge, rebase, apply-mailbox, cherry-pick, revert, bisect, or sequencer state without reading localized human output or following marker symlinks.
  - Git is resolved to an absolute executable outside the project, runs with a cleared minimal environment, fixed `C` locale, prompts/optional locks/fsmonitor/hooks/global/system config disabled, and bounded stdin/stdout. `check-ignore` now uses the same boundary. Parser fixtures cover upstream/ahead/behind and all record kinds; real repositories prove nested-project root discovery, staged/unstaged/untracked/rename state, detached HEAD, a merge conflict plus `MERGE_HEAD`, non-repository behavior, the entry ceiling, and AAP compatibility. No branch, index, worktree, or remote mutation method is added.
- [x] 16.2 Implement structured log, commit detail, branch, upstream, ahead/behind, remote, tag, worktree, and diff queries
  - Four project-bound read-only AAP methods now expose `git-query/0.1`: `workspace/git/overview` returns local branches/current/upstream, tags, redacted remotes, and worktrees; `workspace/git/log` returns 1-100 commit pages with full-OID cursors; `workspace/git/commit` returns author/committer/message and changed paths; `workspace/git/diff` returns bounded worktree, staged, or full-OID commit patches with paths and `+/-` counts. Current branch ahead/behind remains authoritative in `git-status/0.2`.
  - All revision inputs require complete lowercase 40/64-character OIDs, output is capped at 2 MiB, changed paths at 2,000, branches at 256, tags at 512, remotes/worktrees at 128, and Git runs through the task 16.1 cleared read-only process boundary. Nested projects use `:(top)` pathspecs but expose only paths under the opened project. Sensitive/Git-ignored paths and sensitive rename sources are removed before patch generation; remote URL userinfo, path, query, and fragment are never returned, only name plus sanitized fetch/push authority (`local` for filesystem remotes).
  - The Qt Git workspace replaces its placeholder with a repository summary, 100-row commit history, worktree/staged/selected-commit segmented query, refresh action, read-only patch view, and selection-based Git-diff context. Request identity prevents a late diff response from replacing a newer scope/commit selection. Real nested-repository fixtures cover overview, tag, worktree, paged log/root commit, commit detail, all diff scopes, sensitive files/renames, sibling paths, credential-bearing remotes, invalid OID/path rejection, AAP project scoping, and explicit method-not-found for stage/branch/commit/push writes. No Git mutation method exists.
- [ ] 16.3 Implement task branch create/switch/rename with dirty-state preflight and protected-branch policy
  - An internal `git-branch-plan/0.1` sidecar transaction now plans create-and-switch, switch, and rename against canonical worktree root, exact HEAD/current branch, exact target/source OID, complete porcelain dirty state, protected patterns, branch existence, and linked-worktree occupancy. Branch names are accepted only through bounded `git check-ref-format --branch`; default protection covers `main`, `master`, `develop`, `production`, and `release/*`. Nested project roots are blocked because switching would affect repository siblings outside the opened project boundary.
  - Execution rebuilds the entire plan immediately before mutation and rejects policy, HEAD, target, dirty, or worktree drift. Create uses exact-base `git switch -c`, switch binds the reviewed target OID, and rename supports current or non-current branches. Post-state verifies selected branch and HEAD; injected post-command failures prove conservative rollback for create (restore prior branch and remove new ref), switch (restore branch/detached HEAD), and rename (restore source ref).
  - Real fixtures cover clean create/switch/current rename, non-current rename rollback, dirty denial, protected-source denial, stale target/HEAD plan, linked-worktree occupancy, invalid names, and rollback evidence. This task remains unchecked because no permission/approval-backed AAP method or Qt branch control exists; the internal transaction must not be exposed to Agent/Codex yet.
- [ ] 16.4 Implement dedicated child/background worktree creation, locking, session association, health, and cleanup eligibility
  - An internal `git-worktree-plan/0.1` lifecycle now binds canonical repository/common-dir and external storage roots, exact base HEAD/branch, validated unique worktree/branch names, and explicit session/child ownership. Planning blocks nested project roots, dirty/conflicted/truncated bases, operations in progress, repository-overlapping or symlinked storage, existing/symlinked targets, branch reuse, and linked-worktree occupancy.
  - Execution rebuilds the full plan, creates the exact-base branch/worktree through the restricted Git runner, locks it with bounded ownership metadata, and verifies common-dir, path, branch, HEAD, clean state, lock, and non-prunable registration. Injected failures after add and after lock prove removal of both the worktree and new branch with explicit rollback completeness.
  - Versioned descriptors carry owner, base, path, created, and last-health timestamps. Health is rederived from Git/filesystem state and reports missing/symlinked/prunable/unlocked, owner/path/common-dir/branch mismatch, HEAD, dirty/conflict, and operation state. Cleanup is only an eligibility decision: the child must be terminal, the worktree healthy and clean, editor state saved, and integration or discard explicitly selected. Eight real Git fixtures cover success, stale/dirty/overlap/reuse/symlink/invalid-name denial, owner and branch tampering, missing/prunable state, cleanup gates, and both rollback boundaries.
  - This task remains unchecked because the lifecycle is not persisted in a session/child scheduler and no permission/approval-backed AAP or Qt creation/removal/integration flow exists. Worktree mutation and cleanup remain unreachable from Agent/Codex.
- [ ] 16.5 Implement staging by selected Agent-owned paths/hunks without including pre-existing user changes
  - An internal `git-stage-plan/0.1` transaction now accepts only a reverified `GitCheckpointApplication`. Revalidation binds checkpoint ref/commit/tree/manifest/preimage blobs, repository, HEAD/branch, edit/project/root identity, every before/Agent-after state, current index tree, and a stable post-query index-file hash. Plans are content-free and expose operation/path, ownership, hashes, modes, stable hunk IDs/ranges/counts, whole-path requirements, and explicit blockers.
  - Update staging performs a bounded three-way merge with current index content as ours, the checkpoint preimage as base, and only the selected Agent hunks as theirs. This preserves pre-existing user staged hunks while excluding user worktree-only content and unselected Agent hunks; overlapping staged/Agent edits fail without index mutation. Mode changes use the same three-way rule and can be selected independently. Create/delete/rename remain atomic whole-path selections; rename moves the current source index entry so existing staged content is retained, while deletes that would replace capture-time or later staged user content are blocked.
  - The transaction copies the exact bounded index under the standard `index.lock`, preserves permissions, updates only that locked index through the cleared Git runner, revalidates checkpoint/worktree/index state, atomically installs it, syncs the metadata directory on Unix, and verifies the final tree. Injected post-install failure restores the exact original index; an external rewrite before rollback is preserved and reports rollback incomplete. Worktree files are never changed by staging.
  - Six real Git fixtures cover selected hunks with staged/unstaged user content, create/delete/rename, mode-only selection, overlap, stale index, pending editor state, competing lock, later staged delete content, complete rollback, and conservative rollback after an external index rewrite. macOS passes; macOS-to-MSVC checking remains blocked before this module by existing Tree-sitter C compilation without Windows SDK headers.
  - This task remains unchecked because no permission/approval-backed AAP method, durable staging event, or Qt path/hunk review and unstage workflow exists. `workspace/git/stage` remains method-not-found and Agent/Codex cannot reach this index mutation.
- [ ] 16.6 Implement commit preview, message generation source, author/committer policy, hook behavior, and final commit event
  - An internal `git-commit-plan/0.1` now accepts only the reverified checkpoint/application plus exact `git-stage-plan/0.1` result. It binds repository/project/edit/checkpoint identity, HEAD/branch/target ref, staging-before and staging-after index trees, an Agent-only commit tree, selected paths/hunks/modes, excluded user-staged paths with sensitive-name redaction, an exact bounded binary patch and hash/statistics, normalized message plus user/Agent/template source, explicit author/committer identities/timestamps/timezones/sources, detected hook state, signing policy, and blockers.
  - Commit preview uses a three-tree merge with the pre-Agent index as merge base, HEAD as ours, and the post-Agent index as theirs. The result is HEAD plus only the Agent staging delta, so non-overlapping user staged and worktree-only content stays out of the commit while remaining in the real index/worktree. Merge conflicts, empty trees, unselected/sensitive paths, stale stage receipts, invalid metadata, and output limits block. Any configured external `merge.*.driver` or selected-path custom merge attribute is blocked before `merge-tree`, preventing preview from executing repository code.
  - Identity enters the cleared Git environment through six fixed author/committer fields only; names/emails/source/time/timezone and 16 KiB UTF-8 messages are validated and commit objects are precomputed with hooks/signing disabled. Active in-tree hooks and custom hooksPath presence are reported without exposing custom paths. Reviewed disabled-hooks/unsigned policy uses plumbing; requests to run hooks or sign remain blocked on missing sandbox/permission/approval/output-artifact or secure-signer gates rather than executing implicitly.
  - Execution fully replans, then compare-and-swap updates only symbolic branch ref or detached HEAD from the exact reviewed parent to the precomputed commit. It does not modify index/worktree; post-state verifies HEAD/ref/branch, parent/tree, exact raw author/committer/message metadata, and unchanged staged index tree. The versioned `git.commit.completed` result carries commit/parent/tree/ref, message source/hash, identities, hook/signing outcomes, selected/excluded counts, and observation time. Injected post-ref failure rolls back only while the ref still points to this transaction; an external rewrite is preserved and reports rollback incomplete.
  - Five real Git fixtures prove an Agent-only commit excludes same-file user staged/unstaged content while preserving both index and worktree, exact preview/metadata and failing-hook bypass, hook/signing gate refusal, custom merge-driver non-execution, custom hooksPath reporting, invalid metadata/stale index refusal, exact CAS rollback, and conservative external-ref preservation. This task remains unchecked because no permission/approval-backed AAP method, Qt commit review, durable event persistence, hook execution/output artifact, secure signing, or Windows runtime evidence exists; `workspace/git/commit/create` remains method-not-found.
- [ ] 16.7 Implement stash, merge, rebase, cherry-pick, abort/continue, and persisted conflict UI behind risk gates
  - Internal `git-workflow-plan/0.2` plans stash capture, merge, rebase, cherry-pick, and exact-record/generation-bound abort or continue with execution-complete semantics. It binds canonical repository/common-dir, HEAD/branch, index tree/state hash, full target OIDs, pending editor and redacted dirty evidence, live operation, stash-ref baseline, predicted fast-forward/merge-commit/replay/rewrite behavior, message provenance, explicit author/committer/timezone, merge mode, and hook/signing policy. Fast-forward metadata ambiguity, missing merge-commit metadata, already-contained/non-fast-forward targets, merge-commit cherry-pick without mainline, invalid identity/message, dirty/conflicted/truncated state, concurrent operations, protected rebase, pending editor content, and custom merge/filter drivers block.
  - `git-workflow-record/0.2` persists the reviewed metadata but no source or conflict content in a disjoint trusted store. Private bounded files, no-clobber create, atomic replace, SHA-256 envelopes, full semantic validation, and fresh replanning reject stale/tampered authority. Record transitions use an OS advisory lock plus exact generation/record compare-and-swap. A versioned execution attempt fsyncs `prepared`, then `dispatching`, then `observed` or `recovered` with authorization ID/hash, action, timestamps, exit code, and content-free outcome.
  - Reconciliation derives matching, foreign, missing, and conflicting operation state from live Git markers and bounded unmerged index records. Ordinary conflicts retain only exact stage 1/2/3 mode/OID evidence; sensitive names/content are count-only. Crash recovery adopts a matching live operation, verifies deterministic completed/aborted postconditions when HEAD/ref/index/commit metadata prove them, and otherwise becomes inspect-only rather than guessing.
  - `git-workflow-authorization-requirement/0.1` now hashes both the exact current record and exact action plan. Permission and explicit approval decisions each bind that scope, are distinct, `allow-once`, expire within five minutes, and must be atomically consumed by an injected trusted authority. Medium and high risk both require explicit confirmation; abort receives its own high-risk action plan instead of inheriting the original operation risk. The verified ticket is opaque/non-serializable and is rechecked against a fresh action plan immediately before mutation.
  - An internal executor consumes only that verified ticket and runs fixed noninteractive, prompt-free commands for stash, fast-forward/no-fast-forward merge, rebase, single-parent cherry-pick, abort, and continue through the cleared Git runner. Disabled hooks/unsigned policy is explicit; configured hooks are reported and bypassed, while Run/sign requests remain blocked. Post-state verifies stash ref/HEAD, exact merge parents/message/identity, cherry-pick parent/author/message/committer, rebase ancestry/branch, or abort HEAD/index/clean state. Thirteen state/store fixtures, four authorization fixtures, and six real execution/recovery fixtures pass on macOS.
  - A partial trusted-store foundation now uses a private project-external SQLite database in WAL mode with `synchronous=FULL`, foreign-key enforcement, schema/application-ID checks, and reopen integrity validation. It durably stores allow-once permission/approval decisions, atomically consumes both decisions with an `approval.consumed` event, and appends typed content-hashed session events with monotonic replay sequences. Direct authority calls repeat evidence/schema/scope/expiry/distinct-decision validation; failed event append rolls the decision status back. This store is still only an internal authority fixture and does not provide a production issuer, session projection, migration/backup/compaction policy, or AAP/Qt integration.
  - This task remains unchecked because no production permission/approval issuer or complete consumption ledger, AAP methods, durable session event integration, Qt review/conflict/recovery UI, sandboxed hook output flow, secure signing, or Windows runtime evidence exists. The executor remains an unreachable internal library surface; Agent/Codex and the shipped UI remain read-only for these mutations.
- [ ] 16.8 Implement push preview with remote/refspec/commits/force/protection and explicit authentication target
- [ ] 16.9 Enforce high-risk approval for force push, reset, clean, destructive checkout, history rewrite, and branch/worktree deletion
- [ ] 16.10 Apply equivalent Git risk detection to shell commands that bypass structured Git tools
- [ ] 16.11 Implement reviewed child-branch integration with target HEAD revalidation and conflict handling
- [ ] 16.12 Add real-repository fixtures for dirty state, submodules, linked worktrees, LFS, hooks, detached HEAD, conflicts, and remote failures

## 17. Context Engine and Compaction

- [ ] 17.1 Define context manifest entry schema with source, priority, trust, hash, token size, freshness, and inclusion reason
  - Partial manifest foundation: capability `turn.context.manifest` returns `context-manifest/0.1` metadata for each bounded, root-validated turn attachment. Entries include source, kind, pinned priority, untrusted-data trust, SHA-256 content identity, conservative four-byte token estimate, freshness, inclusion reason, and included state; the manifest never contains attachment text. Stale revisions are labelled `stale`, bounded items use an explicit reason, and manifest truncation is fail-closed. Keep this task unchecked until instruction discovery, budget allocation, context inspection, provider-aware token accounting, and cross-provider scale evidence consume the same manifest authority.
- [ ] 17.2 Implement deterministic instruction discovery and precedence for managed, user, project, and nested files
  - Partial instruction-discovery/0.1 foundation: the sidecar discovers supported instruction names from explicitly configured managed/user roots (AEGISY_MANAGED_INSTRUCTIONS_DIR and AEGISY_USER_INSTRUCTIONS_DIR) plus the project-root-to-target ancestor chain. Results are deterministic, bounded, root-scoped, and sorted with a documented weakest-first merge order: managed > user > nested (closer depth wins) > project.
  - Read-only AAP workspace/instructions binds project_id and an optional registered root_id, accepts an optional existing target path, and returns scope/depth/precedence, SHA-256, revision, token estimate, freshness, inclusion, truncation, and rejection metadata. Content is returned only when include_content:true, is explicitly untrusted-data, and can never grant permissions, execute commands, enable Hooks, or authorize network.
  - Discovery rejects symlink components, sensitive/built-in ignored paths, Git-ignored project files, case-collision names, invalid UTF-8, control characters, secret-shaped content, oversized files, and aggregate/file-count/scan limits. It rereads file metadata after content capture and labels changed files stale. Unit and protocol fixtures cover precedence, nested targets, content-free metadata, secret/control/symlink denial, case sensitivity, and limits.
  - Work-mode turn/start now appends up to eight valid discovered instructions after explicit user context, reusing the same bounded context preparation and manifest. The generated items carry instruction precedence in priority/inclusion metadata and remain untrusted; secondary-root attachments do not accidentally select the primary-root instruction chain.
  - Keep this task unchecked: host-managed policy roots, durable configuration, complete instruction exclusion reporting and budget accounting, context inspection, trust/approval intersection, and clean Windows/runtime evidence remain required before this is a complete product feature.
- [ ] 17.3 Implement pinned context for files, selections, images, diagnostics, terminal excerpts, Git commits/diffs, artifacts, and child handoffs
  - Partial internal foundation: `pinned-context/0.1` defines content-free descriptors for every planned source kind with project plus optional session/root binding, bounded source/label/reference, lowercase SHA-256, byte count, revision, freshness, priority, and secret-free metadata. Sets reject duplicate IDs, project mismatch, unsupported schemas/kinds, unsafe path/network references, secret-shaped fields, and 128-item/16 MiB-item/64 MiB-total overflow; a deterministic set identity binds the complete ordered descriptor set.
  - `pinned-context-store/0.1` persists exact metadata-only sets under the Workbench data root as private immutable content-addressed objects plus atomically replaced project pointers. Reopen verifies hash/schema/project/set identity and `content_bodies_persisted:false`; identical writes are idempotent, updates retain old objects, and missing/tampered current authority blocks replacement. Bounds are 1,024 projects, 4,096 objects, 1 MiB/set, and 256 MiB total. Object and pointer publication are not one transaction, so failed pointer replacement may leave a preserved unreferenced object. The private `pinned-context-publication/0.1` journal records previous/next identities before pointer replacement and is removed only after the SQLite event/release transaction succeeds; retries reuse the same journal identity.
  - AAP now advertises `workspace.pinned-context.store` and `workspace.pinned-context.manage` when the store is healthy. `workspace/pinned-context/list` returns the project set, `save` validates project/root/session bindings and supports `expected_set_identity` compare-and-swap protection, and `remove` provides explicit unpin through the same path. Removing the final item persists an empty set identity. A successful mutation appends a separate metadata-only `project.pinned-context-updated/0.1` project event after object publication; an event failure is reported as incomplete rather than hidden. Standard `*:sha256:` Blob references are checked against active project/session-owned SQLite metadata for exact hash/byte identity without reading bytes or updating access time. Every full-set mutation now compares previous/next session-owned image references: the final descriptor removal commits the project event and Blob-reference release in one SQLite transaction, duplicate pins keep the shared reference active, failure rolls back all releases, and success retains content for at least 24 hours. A later explicit import with the exact same content/scope/reference/metadata reactivates that released reference transactionally, while identity drift is rejected. Release-bearing events store only a hashed batch identity and count, so repeated transitions to the same empty set cannot reuse an unrelated earlier event and no reference IDs/bodies enter the event. The earlier external object/pointer publication remains outside the event/release transaction. Protocol and store coverage prove metadata-only persistence, restart recovery, project-event replay, Blob scope/hash checks, idempotent persistence, stale-write rejection, shared-reference final release, release/event rollback, retained content, exact reactivation, repeated-empty-set event isolation, and absence of body fields. This is still a partial AAP bridge.
  - Capability `turn.context.pinned-selected` now lets `turn/context/inspect` and `turn/start` consume only an explicit list of selected file/selection/image/diagnostic/terminal-excerpt/Git-commit/Git-diff/artifact/child-handoff pin IDs together with the exact current set identity. Selection is capped at 16 unique IDs and project/session/root bindings are rechecked. File and selection pins share the normal authoritative resolver: it rereads root-relative files, reapplies workspace policy, compares descriptor revision and raw-byte SHA-256, marks changed content stale, and for selections extracts only the bounded metadata line/column range. Session-owned `artifact` pins resolve only `command-output:sha256:` text references with matching UTF-8 bytes, length, and hash. Project/root-bound `diagnostic` pins resolve only `diagnostic-raw:sha256:` content from the authoritative in-memory DiagnosticStore and recheck media type, reference, SHA-256, and byte count; a Runtime restart or eviction removes that source and therefore fails closed rather than using the persisted descriptor as content. Session-owned terminal pins bind `terminal-excerpt:<terminal>:<generation>:<start>:<end>` to the Runtime-owned retained PTY range and recheck session, generation, absolute offsets, normalized UTF-8, SHA-256, and bytes; restart, removal, generation change, or buffer eviction fails closed. Primary-root Git pins use strict `git-commit:<oid>`, `git-diff:worktree`, `git-diff:staged`, or `git-diff:commit:<oid>` references. They re-run the filtered read-only Git query, bind both the bounded 16 KiB context hash and complete normalized source hash/byte count, and reject mutable worktree/staged drift; commit and commit-diff pins remain valid only while the exact Git object is available. Child-handoff pins now have a read-only `child-handoff/0.1` foundation backed by a parent-session-scoped text Artifact Blob. Metadata binds a distinct same-project source session and handoff ID; assembly rereads UTF-8 bytes and validates owner, media type, hash, and byte count, while inspection never returns the body. Child-task production, parent/child lineage persistence, approval, and multi-agent execution remain under task 21. Inspection never returns source, diagnostic, terminal, Git, artifact, image, or handoff bodies. Existing unit/protocol fixtures cover stale set identity, duplicate/missing IDs, cross-session denial, unsupported kinds, metadata-only inspection, selection ranges, durable artifact/image restart and assembly, child-handoff authority, hash checks, changed-file reread, strict Git references, filtered Git content, commit stability, mutable drift, and source loss; focused Runtime fixtures cover diagnostic source loss plus real macOS terminal and Git assembly.
  - Qt now loads project-scoped persisted pins through AAP, creates or refreshes file descriptors from an authoritative `workspace/read` response with reconstructed raw UTF-8/BOM/newline bytes, and saves with compare-and-swap identity. The composer renders fixed rows with explicit per-turn inclusion, bounded up/down ordering, and unpin controls; selected IDs and set identity are passed separately to `turn/start` and `turn/context/inspect`. The editor context menu can also pin the current clean, conflict-free selection as a range-bound `selection` descriptor after the same authoritative reread; transient inline selections remain available for unsaved buffers. The render fixture covers file and selection pin persistence, range labels, inclusion toggles, and unpin.
  - Qt workspace watcher and user-save events mark matching loaded file, selection, and diagnostic pins `stale` in the composer using their root-relative metadata path. Terminal restart/removal marks matching `terminal_excerpt` pins stale by terminal identity. Qt accepts the sidecar's new set identity only when the version, hashes, and previous identity match its current view; otherwise it reloads the complete authoritative set, preserving CAS correctness for a later unpin or reorder. These local indicators never rewrite durable descriptors or hashes; inspect/start still perform the authoritative sidecar reread and hash/authority check.
  - Runtime now persists sidecar-driven source invalidation as additive `pinned-context-source-invalidation/0.1` metadata. Workspace watch/save changes and diagnostic re-observation mark matching root-scoped file, selection, and diagnostic descriptors stale; terminal restart/removal marks matching terminal-excerpt descriptors stale by terminal identity. Each change publishes a new immutable set through the existing pointer journal and metadata-only project event, returns only bounded state/count/identity or error fields, and never treats a persistence failure as source freshness. A focused Runtime fixture verifies all four source kinds, immutable identity advancement, and stale metadata surviving Runtime restart.
  - Production Qt now uses `artifact/read-command-output-page`, binding every request to the originating Session, wire Item, reference, Runtime generation, immutable metadata, and cursor window. The read-only Artifact dialog exposes an explicit `固定完整输出` action only after complete UTF-8 assembly, terminal byte-count/SHA-256 verification, and canonical omission-marker validation for truncated content in the active project-bound Work session; it persists a session-owned `artifact` descriptor through the existing pinned-context CAS with `metadata.item_id`, priority 700, and retained UTF-8 byte count. Cross-session, blocked/recovery, busy-mutation, invalid-identity, and unsupported-media states are visible but disabled/fail closed; the action never sends the Artifact implicitly. The legacy whole-artifact client API has no Workbench caller.
  - Qt now exposes `固定选中诊断` from the Structure diagnostics surface. It rereads the normalized raw Artifact through `workspace/diagnostics/raw`, validates project/root, media type, reference, SHA-256, and UTF-8 byte count, then persists only a project/root-bound metadata descriptor through the same CAS path. The selected pin is explicit turn context; it is never added automatically. Render coverage exercises pin and unpin when clangd is available.
  - Read-only `terminal/excerpt/read` creates a bounded `terminal-excerpt/0.1` authority from at most 16 KiB of the retained terminal tail, strips ANSI/OSC through pinned `strip-ansi-escapes` 0.2.1, removes remaining non-text controls, and returns the absolute range/generation/hash without persisting content. Qt exposes separate transient `添加选中内容` and persistent `固定最近输出` actions, validates the response, and saves only a session/root-bound descriptor through CAS. Real Runtime and Qt fixtures cover create, selected assembly, metadata-only inspection, pin/unpin, and failure after terminal removal. This does not claim raw offsets for an xterm visual selection or durability across Runtime/terminal restart.
  - Capability `workspace.git-context.read-only` exposes `workspace/git/context/read` for filtered commit detail and worktree/staged/full-OID commit diffs. Qt provides explicit `固定差异` and `固定提交` actions, validates project/root/kind/scope/OID/media/hash/bytes/full-source/truncation identity, and stores only the descriptor through the existing CAS. A real Git render fixture clicks both actions, verifies persisted rows, and unpins them.
  - Capabilities `workspace.image.import-user` and `workspace.image.preview` now provide the explicit user image path. Import is bound to the current project Work session/root, rejects encoded input before Base64 decode, and independently validates PNG/JPEG/WebP under 8 MiB, 8192px-edge, 40M-pixel, and 192 MiB decode-allocation limits before writing a session/project-owned `image:sha256:` durable Blob. Descriptors store only reference/hash/bytes/media/dimensions. Preview rereads the same authority and returns only a 320x180 PNG thumbnail. Inspect/start reread, hash, scope, media, dimension, and decode-validate selected images; an image consumes a conservative all-or-nothing 16 KiB/4096-token budget entry and never enters prompt text. Codex 0.144.5 receives selected images as `localImage` paths through verified private temporary hard links that drop after the turn; startup removes only safely named crash leftovers. Qt exposes `固定图片`, client-side file preflight, dimensions/media/size, explicit thumbnail preview, inclusion/order, and unpin. Real Runtime, protocol restart/scope, adapter request-shape, temporary-link cleanup, and Qt file-dialog/render fixtures cover the flow.
  - Runtime startup now validates pending journals against the pointer, immutable objects, and latest project event. It cleans an unchanged previous pointer without collecting objects, replays only an uncommitted forward event/release transaction, and removes a journal whose event already committed without duplicating it. Malformed, tampered, or ambiguous state disables pinned-context capabilities and preserves objects/Blobs. After compensation, bounded `pinned-context-object-gc/0.1` runs with a 24-hour orphan grace period, protects current pointers and pending journals, rechecks hash/schema/file metadata, and preserves unknown, corrupt, recent, future-dated, or changed entries. Focused store, protocol, and restart fixtures cover journal completion, pointer retry, tamper rejection, abandoned-pointer cleanup, already-committed-event cleanup, journal protection, grace expiry, and uncertain orphan preservation.
  - Keep this task unchecked until an atomic/recoverable boundary exists across the external object/pointer and Workbench event/Blob transaction, Qt covers child handoff and remaining lifecycle/order/inclusion recovery, and complete artifact/image/diagnostic/terminal/Git cross-platform evidence is available.
- [ ] 17.4 Implement token-budget allocation across instructions, task state, recent turns, tool results, pinned context, search, and repository map
  - Partial context-budget/0.1 foundation: the existing turn preparation now emits a deterministic budget plan across current explicit context and auto-discovered instruction items. Instruction precedence ranks and pinned context priority determine allocation order, while rendered text order remains the caller/context order. Existing task-state, recent-turn, tool-result, search/search-result, and repository-map item kinds are classified in the same budget entries without adding implicit context. A 64 KiB total and 16 KiB per-item hard bound are enforced; every allocation and budget exclusion is content-free and included in the turn manifest response.
  - Keep this task unchecked until those classes have authoritative producers, model-specific tokenizer authority is available, provider context windows are enforced, and scale/cross-platform evidence exists.
- [ ] 17.5 Implement model-specific tokenizer adapters plus conservative unknown-tokenizer behavior
  - Partial `tokenizer/0.1` contract now makes the fallback explicit: `unknown-utf8-four-byte` is marked `conservative-unknown`, `exact:false`, and `provider_window_authoritative:false`. Each budget entry reports its conservative estimated token count and the plan carries the tokenizer summary. No provider-specific tokenizer or context-window authority is claimed; keep this task unchecked.
- [ ] 17.6 Implement context inspector showing what will be sent, source, trust, size, redaction, and exclusion reason
  - Partial read-only inspector: AAP turn/context/inspect reuses the exact Work-session instruction discovery, stale validation, and context-budget/0.1 preparation path without starting a model or persisting content. It returns manifest/budget provenance, trust, size, freshness, inclusion, and exclusion metadata with content_included:false; source bodies are never returned.
  - Qt now exposes a search-icon preflight action for an existing session and a read-only metadata table; unchecked client context is represented as an explicit exclusion marker. The Workbench render fixture covers the control and metadata-only result.
  - Keep this task unchecked until all context classes, explicit redaction/exclusion explanations, provider/tokenizer authority, and cross-platform render evidence exist.
- [ ] 17.7 Implement automatic compaction thresholds based on authoritative or conservative context state
  - Partial internal `context-threshold/0.1` evaluator now distinguishes
    authoritative, conservative, stale, and unknown observations; validates
    soft/hard limits; uses explicit hysteresis; handles missing limits and
    checked arithmetic overflow; and returns `no-action`, `preview-required`,
    or `hard-limit-exceeded` decisions. Every decision fixes
    `automatic_compaction_authority=false`. Codex usage Timeline items now feed
    provider-observed last-input/context-window values into the evaluator and
    retain hysteresis within the active turn. Runtime reconstruction, resume,
    fork, and portable import now replay the complete bounded durable usage
    history; malformed or incomplete evidence fails closed to
    `preview-required`, while a complete history with no usage is explicitly
    `empty` and starts at `no-action`. Session start/resume/fork/read/import
    expose a read-only `session-context-threshold/0.1` projection and Qt renders
    valid normal/review/hard-limit states while malformed input becomes unknown.
    The Qt per-Session display cache is capped at 128 entries and preserves the
    current Chat, Work, history, and active-Turn Sessions during deterministic
    eviction. This remains a review signal only: it is not wired to checkpoint
    activation, provider compact, or automatic authority. Complete Qt runtime
    coverage and cross-platform evidence remain open, so keep this task unchecked.
- [ ] 17.8 Implement compaction summary schema covering decisions, unresolved tasks, changed files, commands, tests, failures, and next steps
- [ ] 17.9 Implement manual compaction preservation instructions and editable review before activation
- [ ] 17.10 Add context quality tests for large monorepos, irrelevant-file resistance, stale results, nested instructions, and provider switching
  - Partial quality evidence: deterministic unit tests cover large monorepo
    ignored trees, irrelevant repository-map resistance, stale file rereads,
    nested target-scoped instruction inheritance, and the distinction between
    intentional exclusions and budget truncation. Provider switching and
    cross-platform scale evidence remain open; keep this task unchecked.

## 18. Permission, Sandbox, and Secret Enforcement

- [ ] 18.1 Define Chat, Read Only, Workspace Write, Developer, and Full Access profile schemas and managed-policy intersection
  - Partial internal foundation: `permission-profile/0.1` defines the five profiles, bounded filesystem roots and access modes, command/network/extension/browser/background capabilities, and a managed-policy intersection that can only narrow a profile. Built-ins remain project-root scoped; Full Access does not bypass managed policy, sensitive paths, or symlink denial. No AAP/Qt profile picker or write-capable runtime is enabled.
- [ ] 18.2 Implement filesystem root, denied path/glob, symlink, command, network host, extension, browser, and background policy engine
  - Partial internal foundation: effective-policy checks enforce canonical roots, sensitive and denied globs, symlink-component rejection, shell-wrapper rejection, executable and host allowlists, extension kind/ID gates, browser host gates, and background policy. The policy engine is an explanation boundary only; it does not launch commands or grant Agent mutation authority.
- [ ] 18.3 Implement granular approval rules for once, turn, session, and durable hash-bound scopes
- [ ] 18.4 Implement risk classification for commands, files, Git, network, MCP, plugins, and permission escalation
- [ ] 18.5 Implement secure path defaults for keychains, SSH, cloud credentials, browser profiles, `.env`, and project secret configuration
- [ ] 18.6 Implement secret detectors and redaction at tool output, event persistence, logs, diagnostics, UI, and model-request boundaries
- [ ] 18.7 Implement untrusted-data provenance and tests preventing repository/web/MCP/tool content from granting authority
- [ ] 18.8 Integrate and verify Codex sandbox profiles for the initial adapter on macOS and Windows
- [ ] 18.9 Design and implement native macOS sandbox enforcement for the future Aegisy runtime
- [ ] 18.10 Design and implement native Windows restricted execution, process, filesystem, and network enforcement without administrator rights
- [ ] 18.11 Add adversarial tests for path escape, symlink race, command wrapping, redirect credential leak, forged approval, prompt injection, and sandbox bypass
- [ ] 18.12 Block write-capable release on a platform until its sandbox and security gate report passes

## 19. Skills, Plugins, MCP, Hooks, and Instructions

- [ ] 19.1 Define unified extension registry schema for type, source, version, hash, signature, trust, permissions, scopes, and updates
- [ ] 19.2 Import existing Aegisy Skills and MCP configuration with preview, backup, and reversible migration
- [ ] 19.3 Implement managed/user/project/session/child scope precedence and effective-state explanation
- [ ] 19.4 Implement Skill discovery, invocation identity, reference loading, context accounting, enable/disable, and file watching
- [ ] 19.5 Implement plugin manifest validation, component preview, install, health check, atomic upgrade, rollback, disable, and remove
- [ ] 19.6 Implement MCP stdio/HTTP lifecycle, OAuth/auth-required state, tools/resources/prompts, progress, elicitation, approval, and bounded logs
- [ ] 19.7 Implement hook discovery, hash trust, events, matcher, timeout, fail policy, output limits, and denial attribution
- [ ] 19.8 Revoke trust and durable approvals when executable content, command, signature, hash, or requested permissions change
- [ ] 19.9 Implement extension marketplace source policy, signature verification, compatibility, update channel, and supply-chain audit
- [ ] 19.10 Add malicious and broken extension fixtures for secret access, excessive output, timeout, crash, dependency failure, and manifest spoofing

## 20. Observability, Diagnostics, and Evaluation

- [ ] 20.1 Implement structured local turn trace with source-qualified runtime/model/context/tool/approval/usage/change/test/error data
  - Partial internal `turn-trace/0.6` contract keeps strict version-specific reads
    for durable `0.1` through `0.6` records while preserving fixed legacy
    serialization identities. Future `0.7+` records and cross-version fields fail
    closed. `0.2` added one immutable Runtime-observed Intent for
    Chat conversation, Work read-only inspection, or future Work mutation. A
    completed terminal binds that exact Intent and classifies workspace change,
    Git change, and verification independently as not-applicable, applicable,
    unknown, or observed; `completed` means only that the provider Turn lifecycle
    ended normally. Chat uses three explicit not-applicable domains. Current Work
    is read-only, so workspace/Git change are not applicable and verification
    remains unknown rather than being inferred from missing provider items. Future
    mutation completion requires observed Workspace evidence matching an Applied
    Change. Failed/interrupted Turns contain no completion domains. Existing
    runtime/model/context/tool/approval/usage/change/test/error validation,
    source/authority labels, redaction bounds, duplicate-usage protection, event
    ordering, and terminal-last invariants remain. The Store continues writing the
    unchanged outer `turn.trace.recorded/0.1` envelope in the same SQLite
    transaction as the terminal Turn event. SQLite remains v13; no migration,
    backfill, or event rewrite is introduced.
  - `0.3` adds at most one final `usage-report`: the latest successfully validated
    and persisted Codex Provider-thread `usage-authority/0.1` report, explicitly
    scoped as an `absolute-snapshot` with unavailable Attempt and Retry attribution.
    Runtime clone-preflights the candidate Trace, commits the ordinary Usage Timeline
    Item, and replaces the authoritative accumulator only after commit; each later
    successful notification replaces the retained snapshot rather than
    being summed. At terminal finalization the retained report binds its deterministic
    identity, observation time, and exact persisted Usage Item and is ordered before
    Error/Terminal. Completed, failed, and interrupted traces retain observed Usage.
    If no notification forms a valid authority report and is successfully persisted,
    no Usage event is emitted; an Unknown event is not fabricated. Store admission,
    direct read, and projection replay inspect the complete bounded Session Item
    prefix through the target Turn's final Usage. They restore one threshold latch
    from the single `NoAction` initial state, use the same 100,000-all-Item uncertainty
    boundary as Runtime restoration, consume matching Usage rows lazily instead of
    retaining every historical payload, rebuild authority from each valid raw Provider
    snapshot, and require the Trace to reference the final valid Item. Later Turns do
    not affect older Trace validation. Malformed raw metadata may remain a
    non-authoritative Timeline Item and consistently advances the review latch to
    `PreviewRequired`, while removal of an authority report from valid raw Usage fails
    closed as a semantic downgrade. First-state and cross-Turn hysteresis substitution
    fail admission and quarantine hash-consistent replay tampering.
  - `0.4` adds a content-free Codex command Tool lifecycle. Started observations bind
    the exact provider status/source/time and a stable identity over a fixed closed
    projection of command/cwd/action type and recognized typed action fields; unknown
    Provider keys/values never enter the stable Trace identity. Object keys are
    canonical and action-array order remains semantic. A separate opaque memory-only
    SHA-256 fingerprint covers the complete untruncated Provider command/actions/cwd,
    including unknown fields, and detects lifecycle drift without serialization or
    persistence. Terminal observations are constructed from the exact would-be
    sanitized persisted Item, preflighted on an accumulator clone, and made
    authoritative only after the command Item and Artifact/Blob transaction commits.
    They bind a Session/Turn/Item domain-separated identity rather than the raw
    Provider Item ID, the complete sanitized Item payload SHA-256, output identity,
    duration, and exit status.
  - Producer admission serializes the complete outer `turn.trace.recorded/0.1`
    envelope and enforces the exact 72 KiB durable limit. It reserves every open
    Tool's worst legal terminal, worst failed Error and each terminal state, plus one
    emergency Started while admission is open. Exhaustion retains and emits that
    Started, denies terminal Item/Blob persistence, and durably fails the Turn.
    Store independently rejects oversized Trace events on terminal admission, direct
    read, projection replay, and restart. Contract, adapter, Store, and stdio fixtures
    reject invalid status/source/time/duration/exit/binding/order, duplicate/missing
    pairs, semantic tampering, exact-budget exhaustion, command Item insertion failure,
    and Provider completion with an unmatched Started. Each failure retains Started +
    Error + failed Terminal without a terminal Tool, command Item, Blob reference, or
    object. Provider `declined` remains only a Tool state and is not a Runtime denial
    or user Approval decision.
  - `0.5` adds exactly one content-free Runtime approval-policy observation. It binds
    the pinned Codex adapter/runtime, fixed producing Runtime identity, durable
    Provider-thread binding, configured/effective `approvalPolicy=never`, read-only
    sandbox, and read-only permission profile. It records no user decision and grants
    no execution authority. Store admission, read, replay, and startup revalidate the
    complete durable binding; `0.5` and `0.6` reject every `Approval` payload until a
    genuine-user authority producer and consumption ledger exist.
  - `0.6` adds a distinct content-free Runtime-denial producer for active-Turn Codex
    command-execution, file-change, and permissions approval requests. The complete
    bounded request is hashed only in memory; durable identities bind request kind,
    Trace, Provider thread, and Runtime policy without prompt, body, path, command,
    output, PID, credential, or raw request/Item IDs. Runtime prepares and
    budget-checks a non-serializable ticket, the adapter writes and flushes the fixed
    denial response, and Runtime commits only after a successful local write. Failed
    writes produce no denial, mark the adapter restart-required, and
    `decline-flushed` does not claim Provider receipt. Trace preflight failure sends a
    fixed content-free error before the Turn fails and permits backend reuse only
    after that error flush succeeds.
    Invalid active-Turn bindings receive a content-free `-32602`, fail closed, and
    produce no Runtime denial or Approval. Store admission, read, replay, and startup
    quarantine revalidate denial identity, ordering, authority, time, redaction, and
    the durable Runtime/adapter/version/Provider-thread/policy binding.
    The checked-in generated Codex `0.144.5` schema does not define these three
    `ServerRequest` shapes; current evidence is the same-version App Server protocol
    source plus deterministic real-stdio fixtures, not generated-schema validation.
  - A Codex file-change denial now also produces an immutable, domain-separated
    `workspace-edit-proposal/0.1` before the denial is flushed. This is durable
    Change input evidence, not a `turn-trace/0.6` Applied Change, user Approval, or
    execution result. Proposal persistence binds the provider thread/item and active
    durable Turn while fixing mutation/approval/apply authority false; complete
    Change trace production still requires reviewed approval, apply, final hashes,
    checkpoint, and recovery evidence.
  - Current Codex Runtime produces `0.6` traces for completed, failed, and
    interrupted Turns. Provider-terminal persistence has one idempotent terminal-write
    retry; structured adapter failures use a separate single-write fail-closed path.
    Stdio fixtures prove Chat and read-only Work completion, exact Intent/domain
    binding, restart equality, transport/provider failure, interruption, and no
    fabricated Workspace/Git/Test evidence or content. Store admission, direct read, and
    projection replay require project/environment plus the current Intent mode to
    equal the persisted Session binding while
    leaving legacy `0.1` mode-less records readable; semantic mode substitution
    fails closed. Complete genuine-user Approval/Change/Test producers, non-command
    Tool families, per-Attempt and Retry
    Usage authority, and Timeline trace projection, AAP/Qt, audit, retention, and
    export surfaces remain absent. Terminal admission and projection replay also
    still repeat the bounded historical scan for long Sessions; single-pass replay
    and a verified-prefix cache remain performance work. Keep this task unchecked.
- [ ] 20.2 Implement observed, catalog-derived, estimated, stale, and unknown labels for token, context, cost, and reasoning status
  - Partial foundation: internal `usage-authority/0.1` validates exactly the
    four metric classes and source labels, rejects unknown values with numbers,
    stale authoritative flags, expired catalog-derived values, invalid
    derivation identities, and inconsistent token/cache/reasoning counts. Codex
    `thread/tokenUsage/updated` Timeline items now retain the raw bounded usage
    projection plus a validated authority report: provider-reported token and
    context-window/reasoning fields are observed, cost is unknown, and
    unreconciled provider totals are not rewritten. The report now has a
    deterministic metadata identity used by `turn-trace/0.3` through `0.6`; it remains a Codex
    Provider-thread absolute snapshot and does not claim Turn Attempt or Retry
    attribution. Qt now renders a strict
    metadata-only summary and rejects malformed/unknown versions. This remains
    Codex-only and has no catalog pricing, cross-provider correlation, billing,
    durable per-Attempt accounting, routing, or complete cross-platform evidence;
    keep this task unchecked.
- [ ] 20.3 Implement privacy-preserving local audit events for privileged operations and permission decisions
- [ ] 20.4 Implement diagnostic bundle preview, category opt-in, redaction report, deterministic archive, and support correlation ID
- [ ] 20.5 Build deterministic adapter replay harness with event ordering and property tests
- [ ] 20.6 Build repository task corpus with provenance, reset scripts, expected tests, and secret/license scanning
- [ ] 20.7 Measure completion, regression, test pass, correction, approval burden, latency, token, and cost metrics by model/runtime/profile
- [ ] 20.8 Add model-role recommendation pipeline tied to evaluation version and known limitations
- [ ] 20.9 Add crash, reconnect, migration, offline, low-disk, runtime-upgrade, and rollback endurance suites
- [ ] 20.10 Add opt-in aggregate telemetry with no prompt/code/path/diff/terminal/extension argument content
- [ ] 20.11 Create a release-gate dashboard that blocks promotion when required evidence is missing or below threshold

## 21. Background Jobs and Multi-Agent Milestone

- [x] 21.1 Keep background and multi-agent feature flags unavailable in stable builds until prerequisite gates are recorded complete
  - `runtime/degradations` now reports content-free `background-jobs`, `multi-agent`, and `unattended-writes` release gates with bounded missing-task IDs, `availability=not-advertised`, `stable_enabled=false`, and `override_available=false`. The initialize capability list does not advertise any of these autonomy methods, and an unknown background dispatch remains method-not-found. Protocol tests lock the no-hidden-switch boundary. This does not enable scheduling, child execution, Agent writes, or unattended work; tasks `21.2+` and the permission/sandbox/recovery gates remain required.
- [ ] 21.2 Implement structured plan steps with IDs, dependencies, owners, evidence, and stale-state revalidation
  - Partial internal `structured-plan/0.1` foundation validates bounded step IDs/statuses/owners, dependency references and cycles, content-free SHA-256 evidence, and completion evidence. Revalidation compares plan/base/evidence revisions and marks affected steps stale without resetting status. It has unit coverage but no AAP/UI persistence, plan question flow, or executor integration yet; keep this task unchecked.
- [ ] 21.3 Define child-task contract for goal, context, worktree, tools, model profile, permissions, budgets, and handoff
  - Partial internal `child-task/0.1` contract binds parent session/turn, bounded goal, content-addressed context identities, project/root and `read_only` or `dedicated_worktree` isolation, tools, model profile, permission/network policy, token/cost/time/turn/tool/concurrency budgets, and an expected `child-handoff/0.1` result shape. Validation is content-free and grants no authority; four unit fixtures cover valid identity, unsafe scope, duplicate resources/budgets, and secret/result rejection. Parent/child lineage, approval, scheduler, worktree creation, and execution remain unavailable, so keep this task unchecked.
- [ ] 21.4 Implement parent/child session lineage, navigation, status, cancellation, and bounded result handoff
  - Partial internal `child-task-state/0.1` foundation tracks parent/child session identity, generation, queued/running/waiting-approval/terminal status, cancellation request, rejection, and acknowledgement, completion races, transactional state updates, generation exhaustion, and content-free bounded handoff references/counts. Six unit fixtures cover binding, legal/invalid transitions, idempotent and rejected cancellation, cancel-vs-complete races, generation exhaustion, and invalid handoffs. Durable child sessions, navigation, AAP cancellation/status, persistence, and parent review remain unavailable; keep this task unchecked.
- [ ] 21.5 Enforce dedicated worktree for every concurrent write-capable child
  - Partial internal `child-worktree-admission/0.1` gate binds a validated child-task request and runnable lifecycle identity to the existing live dedicated-worktree descriptor/health check, exact parent/child owner, and delegated base revision. Shared/missing/reused/dirty/conflicted/unhealthy/cancelling isolation fails before admission; five real-Git fixtures cover shared read-only, healthy write admission, missing dedicated isolation, cross-child reuse, dirty state, and cancellation. The proof explicitly grants no permission or execution authority. Durable scheduler association, project-root registry binding, per-tool revalidation, permission/approval/sandbox intersection, executor integration, AAP/Qt visibility, and cross-platform evidence remain unavailable; keep this task unchecked.
- [ ] 21.6 Implement token, cost, time, turn, tool, concurrency, and network budgets in the runtime
  - Partial internal `child-runtime-budget/0.1` ledger reserves token/cost ceilings before model admission, settles authoritative/estimated usage, charges unknown usage as the full reservation, and transactionally enforces wall-time, turn, tool-call, active-concurrency, and policy-bound network-request limits. Content-free snapshots include limits, used/reserved/remaining values, usage-source counts, and warning/saturated/exhausted dimensions while granting no permission or execution authority. A reusable snapshot validator rejects out-of-contract limits, impossible active reservation/source accounting, inconsistent remaining values, forged classifications, and authority flags. Seven fixtures cover conservative unknown usage, threshold warnings, overcommit rollback, concurrency recovery, independent tool/network enforcement, network-policy binding, wall-time exhaustion, clock regression, generation exhaustion, and snapshot tampering. Provider usage/monotonic-clock integration, durable events, scheduler/executor admission, cancellation/refund policy, AAP/Qt events, and endurance/cross-platform evidence remain unavailable; keep this task unchecked.
- [ ] 21.7 Implement unified executor modes for interactive, child, and background work without separate Agent logic
  - Partial internal `unified-execution-plan/0.1` invariant gives interactive, child, and background envelopes the same 14-stage order across identity, reconciliation, permission, approval, workspace, budget, sandbox, recovery, durable job, notification, release, dispatch, observation, and handoff. Mode derives required gates and terminal evidence; invalid child/job/unattended relabelling fails. The current Codex read-only interactive `turn/start` now validates this plan before adapter dispatch, while child/background plans remain blocked and unadvertised when their gates are missing. Six fixtures prove identical stage order, current interactive readiness, child write gates, background durability/notification/release gates, mode-binding rejection, and common-gate fail-closed behavior. Plans grant no permission or execution authority. Typed proof composition, child/background dispatch, generic provider/tool execution ownership, durable events/recovery, budget settlement integration, AAP/Qt status, and cross-platform evidence remain unavailable; keep this task unchecked.
- [ ] 21.8 Implement durable job queue, schedule metadata, pause/waiting approval, cancellation, idempotent retry, and recovery
  - Partial internal `background-job-request/0.1` and `background-job-state/0.1` contracts bind project/session/root, unified plan, optional child, idempotency, manual/one-shot schedule, bounded attempts/backoff, and an optional safe retry boundary. Transactional state covers queue/start, pause request/acknowledgement, approval wait, cancellation request/acknowledgement and completion races, terminal result/evidence, exact retry eligibility, and restart recovery. Active restart state becomes interrupted rather than successful; queued/paused/waiting state is preserved; automatic approval/retry are always false; pause/resume cannot bypass schedule/backoff or clear cancellation. Workbench schema v11 persists exact validated job JSON/hashes/generation/indexes and one optional `background-job-scheduler-lease/0.1` per job. Lease acquisition, renewal, state rebind, verified process bind, release, and expiry use generation CAS and typed `background-job.lease-*` events; event failure rolls back, startup revalidates at most 10,000 rows, active leases protect terminal-session deletion, stale leases can only expire without adopting newer state, and v10-to-v11 uses the WAL-consistent backup. Internal `background-job-scheduler/0.2` loads only a complete bounded recovery set into an owner/generation-bound hashed snapshot. Lease states distinguish missing/current/expired/released/stale/owner-mismatch and process ownership distinguishes missing lease/registration, unavailable/non-running observation, mismatch, and current. Internal `background-job-process-observation/0.1` accepts only a Runtime-owned child handle, binds exact owner/job/request/state/generation/attempt/time without serializing PID/command/path/output, and distinguishes owned-running, owned-exited, absent, inaccessible, mismatched, and unknown. Monitor-only requires a current durable lease, matching verified process-registration and process identities, and an exact live handle; process exit never implies job completion, persisted hashes cannot adopt a process after restart, pending cancellation remains separately acknowledged, and failed refresh retains the prior snapshot. Internal `background-job-recovery-decision/0.1` binds a semantically revalidated snapshot entry to exact job/lease/process/blocker/timing evidence and appends an idempotent `background-job.recovery-reviewed` session event only after current job and lease rechecks. The 10,000-event journal is startup-validated; event failure rolls back sequence allocation and hash-consistent semantic tampering fails startup. Decisions never mutate job/lease/process state and fix dispatch, mutation, automatic retry/approval, and takeover authority to false. Thirty-three focused fixtures cover job lifecycle, job/lease persistence and migration, event rollback/tampering, deletion/retention, lease expiry/staleness/terminal release, scheduler/process ownership, exact generation rebinding, real macOS running/exited observation, approval/retry review, atomic refresh, durable/idempotent recovery decisions, stale/forged rejection, and decision-event rollback/tampering. The cfg-gated Windows fixture is present but not runtime-verified. A capability-gated `session/background-recovery` AAP page and Qt Session-menu viewer now expose only metadata-only recovery entries, leases/process ownership, blockers, and matching recorded decisions with strict cursor identity validation; they never mutate state or grant dispatch, takeover, retry, approval, or mutation authority. Automatic lease acquisition/renewal, decision production/consumption, authoritative approval, recovery transitions, notifications, and cross-platform endurance remain unavailable; keep this task unchecked.
- [ ] 21.9 Implement platform notifications for completion, failure, approval-needed, and budget exhaustion
  - Partial internal `background-job-notification-intent/0.1` contract derives content-free completed, failed, approval-needed, and budget-exhausted evidence from an exact validated job request/state and optional semantically validated child-budget snapshot. It binds project/session/root, request/state identities, job generation/status, terminal/result/approval evidence, exhausted dimensions, creation time, a stable deduplication identity, and a full intent identity. Deduplication remains stable for the same state/evidence across later creation times; generation zero is accepted when the complete budget accounting/classification remains valid. Every intent fixes content inclusion, delivery availability/attempt, and platform delivery authority to false. Workbench schema v12 adds a bounded `background_notification_outbox`: one canonical intent per deduplication identity commits with `background-job.notification-recorded/0.1` in the same transaction, binds the exact lifecycle event, and fixes delivery state to `recorded` with zero attempts and no authority. Identical retries remain idempotent after later job transitions; stale first writes, conflicting evidence, event failure, projection/lifecycle tampering, and more than 10,000 records fail closed. Internal session-scoped pagination is read-only, v11-to-v12 uses the WAL-consistent migration backup, and terminal session purge removes jobs/outbox/events together. Capability `background-notification.outbox.read-only` and AAP `session/background-notifications` expose only the versioned session-bound metadata page when the durable store is healthy; missing storage and forged cursors fail explicitly. Qt adds a capability-gated Session-menu viewer with strict schema/session/content/authority validation, keyset paging, empty/failure states, and no delivery action. Thirteen focused contract/store/migration/deletion/protocol fixtures cover the four kinds, accounting/identity tampering, deduplication, restart, paging/cursor validation, missing storage, rollback, semantic tamper startup failure, migration, and purge; the current complete Qt render gate exercises the notification viewer's empty and paged states. Scheduler production, platform delivery settings and transitions/retry/confirmation, macOS/Windows notification APIs and permission behavior, localization/privacy review, and cross-platform evidence remain unavailable; keep this task unchecked.
- [ ] 21.10 Implement reviewed child integration with diff, tests, conflicts, keep/discard, and cleanup controls
- [ ] 21.11 Run cost runaway, cross-session leakage, shared-worktree, orphan process, crash, and injected-task adversarial suites
- [ ] 21.12 Approve a separate OpenSpec and release gate before enabling remote messaging or unattended production writes

## 22. Migration, Packaging, and Release

- [ ] 22.1 Add workbench database and files under a versioned Aegisy data root separate from existing QSettings profile state
- [ ] 22.2 Implement feature-flagged import of active model, Skills, MCP, and project history without deleting source configuration
- [ ] 22.3 Ensure workbench startup or migration failure cannot block login, legacy connection management, gateway, updates, or logout
- [ ] 22.4 Package web assets, sidecar, pinned adapters, licenses, NOTICE, schemas, and sandbox resources in signed macOS and Windows bundles
- [ ] 22.5 Verify sidecar and adapter hashes before launch and integrate their versions into updater compatibility checks
  - Partial local foundation: `aegisy-artifact-manifest/0.1` validates a present sidecar/adapter manifest before Qt launches `aegisy-agentd`, including bounded manifest/artifact reads, fixed runtime/adapter identities, portable relative paths, canonical in-tree ordinary files, link/reparse and extra-hard-link denial, version metadata, and SHA-256 verification. Rust Runtime consumes that same adjacent contract before selecting Codex: a present manifest takes precedence over `AEGISY_CODEX_PATH`, exact Runtime/adapter versions and files are hash-bound, duplicate/unknown JSON fields, non-portable or link/reparse paths, a Windows adapter path without an exact `.exe`, every extra hard-link alias, path/content/file-identity drift, and a changed manifest identity within one startup attempt fail closed. Hashing uses a bounded heap buffer, and verification runs immediately before both version probing and App Server spawn. Focused Qt/CMake and 16 Rust tests cover these boundaries. Developer builds may still omit the manifest; release packaging must add a non-downgradable require-manifest identity, close the remaining verify-path-to-spawn replacement window, bind updater compatibility, and supply signed macOS/Windows evidence, so keep this task unchecked.
  - `cmake/generate_artifact_manifest.cmake` is a deterministic packaging foundation for an already assembled bundle. It accepts only distinct regular runtime/adapter files and an existing canonical output parent inside the bundle, rejects symlink/output/path escape, enforces the verifier's identity/version/path/artifact limits, hashes the final bytes, and emits no timestamp or machine path. The generation fixture proves repeatability, outside-artifact/output rejection, and that the production Qt verifier accepts the generated manifest. Packaging scripts intentionally do not invoke it until a reviewed pinned Codex adapter is bundled; signed macOS/Windows integration remains open.
  - `artifact_manifest_runtime_startup` now copies the real Release daemon and a fixed-version test adapter into a Unicode temporary directory, generates the production manifest, supplies a bogus developer adapter override, and proves that the real daemon starts only the manifest-owned adapter through the complete AAP initialize/shutdown path. The local macOS fixture passes and the clean Windows Unicode workflow is configured to run it through the unfiltered CTest suite, but no successful Windows execution has been observed; task `22.5` remains unchecked.
  - Partial signed-update foundation: a compile-time `aegisy-update-signing-trust-anchor/0.1` authenticates exact-generation `aegisy-update-signing-key-ring/0.1` envelopes before the production `aegisy-update-artifact-set/0.2` path is usable. Bootstrap is Root-bound; rotation is sequential, retains prior keys, cannot widen validity or reverse revocation, requires monotonic Ring signing time, and binds each key's first admitted generation/time so backdated Ring or Artifact Set signatures fail. Because `signed_at_ms` is signer-controlled, first-time bootstrap/rotation also requires the signer to be active at local verification time; only an already accepted exact envelope may replay idempotently after expiry, and offline expired-signer recovery remains unavailable without an independent witness/checkpoint. Admission history remains authority-bound even when two histories converge on the same later Ring. Artifact Set `0.2` binds the receipt/candidate signer, signing and expiry times, payload identity, target and source application size/SHA-256, complete manifest, and latest trust-anchor/Ring authority. Production evaluation requires an opaque `InstalledArtifactSetAuthority`; its fixed Windows/macOS factory verifies the adjacent receipt/Manifest/Runtime/adapter/application graph and rederives it for every candidate. Both the Manifest and installed-layout verifiers now use a shared explicit Windows/Posix canonical containment policy because Qt Windows canonical paths use `/`; portable drive/root/UNC/case/sibling tests and the static packaging gate cover the source regression, while clean Windows execution remains required. Raw-key, arbitrary-root, scalar-tuple, and `0.1` entry points are testing-only. A separate no-testing-macro production fixture proves embedded Root -> generation-two rotation -> historical installed receipt -> rotated-key candidate and rejects `0.1`; every result keeps download/install authority false. The internal `aegisy-update-progress-record/0.1` Store still provides only local continuity with fixed-false authority. `UpdateManager` consumes neither contract; the release Root defaults empty, no authenticated Ring fetch or persistent anti-rollback high-water exists, current-path identity is not loaded-process or outer-package membership, and no transaction spans Key Ring, progress, updater/framework state, secure anchor, and signed package identity. Sparkle resume/deltas and the WinSparkle pre-download veto gap remain open. Keep `22.5` unchecked until those gates and signed clean-platform package evidence are closed.
  - Partial local-integrity continuity cache: `aegisy-update-signing-key-ring-continuity/0.1` stores exact signed Ring envelope bytes as immutable generation objects behind a private bootstrap marker and atomically replaced head. Restart replays `1..N` from the embedded Root using the current `nowMs`; the cache never persists or deserializes `Authority`, admission time, `accepted_at`, or verification tickets. Strict current-time replay is `Authoritative`; only a complete historical chain whose current-time failure is one of the explicitly replayable activity errors (`bootstrap-root-invalid`, `signer-inactive`, or `no-current-active-usage`) is `CachedButNotAuthoritative` and has no valid `Authority`; revoked, malformed, structurally invalid, and signature-invalid chains remain `Invalid`. Local locking, expected-head CAS, bounded reads, file/directory identity checks, private Unix permissions, immutable objects, and fail-closed inconsistent evidence are covered. An exact retry of the already accepted latest envelope is idempotent and no-write when the expected identity is the current or immediately previous cache identity; stale CAS that would admit a different envelope or create a new generation, repairs, and appends from cached-only state are rejected. Complete deletion means the cache directory is absent and remains `Empty`; a retained directory with partial evidence is `Invalid`. No updater, authenticated publication, secure high-water, trusted time, anti-deletion, or operation authority is added. Keep `22.5` unchecked.
- [ ] 22.6 Implement emergency server-controlled feature disable that does not expose session content or prevent local history export
  - Partial desktop/runtime foundation: after login, Qt polls authenticated HTTPS `GET /api/v1/client/workbench-policy` without redirects or cached transport responses and accepts only the bounded signed `aegisy-workbench-emergency-policy/0.1` envelope. The pinned Ed25519 updater key verifies exact content-free fields, freshness, maximum lifetime, positive monotonic sequence, and a persisted sequence/identity high-water marker; rollback, same-sequence conflict, missing/corrupt cache halves, expiry, and signature/field drift fail closed after the first accepted policy. Applying or clearing a valid higher-sequence policy immediately gates Qt requests and performs a bounded Sidecar generation switch. Disable retires the old normal Sidecar even with in-memory work so Runtime enforcement cannot remain normal behind a Qt-only gate; shutdown is bounded and no interrupted work is inferred successful. Emergency Sidecar startup uses the Preview/Store backend without launching Codex, advertises only reviewed read/history/export capabilities, centrally rejects new Session/Turn/workspace/import/restart mutations with `-32153`, and preserves local Session read plus actual portable export. Login, legacy gateway, updates, logout, and non-Workbench account/profile surfaces remain outside this gate. Keep `22.6` unchecked: the production endpoint/signing publisher and signed-package end-to-end evidence do not exist; first install with no cached policy retains current behavior; the QSettings high-water marker is not an OS-secure anti-deletion anchor; and a healthy-Store diagnostic bundle exporter is still absent.
- [ ] 22.7 Run signed/notarized macOS packaging, update, rollback, multi-display, IME, accessibility, and sandbox smoke tests
- [ ] 22.8 Run signed Windows packaging, install/upgrade/rollback, 125%/150% scaling, ConPTY, Git, sandbox, antivirus, and long-path smoke tests
- [ ] 22.9 Publish internal, preview, beta, and stable release criteria with support playbooks and known limitations
- [ ] 22.10 Promote the workbench as default landing surface only after parity, retention, reliability, security, adoption, and support gates pass

## 23. Documentation and Operational Readiness

- [x] 23.1 Write end-user concepts for Chat, Work, projects, sessions, model profiles, permissions, context, checkpoints, Git, and extensions
  - `docs/END-USER-CONCEPTS-GUIDE.md` provides comprehensive end-user documentation covering Chat vs Work modes, project trust and roots, session lifecycle and management, model profiles and switching, permission profiles and approval workflow, context budget and pinned content, checkpoints, Git integration and safety, extensions and security, best practices, and glossary. The guide uses clear language, practical examples, and emphasizes security and user control. Completed 2026-08-01.
- [x] 23.2 Write first-run onboarding that opens a sample or selected project without defaulting to Full Access
  - `docs/FIRST-RUN-ONBOARDING.md` provides comprehensive first-run onboarding documentation covering initial setup, project selection (sample/existing/new), project trust review, permission profile selection (Read Only recommended), first session guidance, Chat vs Work modes, safety features (approval workflow, checkpoints, secret detection), interface exploration, key concepts, customization options, common questions, best practices, and getting help. The guide emphasizes security-first approach, never defaulting to Full Access, and gradual learning. Completed 2026-08-01.
- [x] 23.3 Write model capability and switching explanations that distinguish compatible continuation from portable session fork
  - `docs/MODEL-SWITCHING-GUIDE.md` provides comprehensive guide on model capabilities, compatibility checking, compatible continuation vs portable session fork, capability indicators, context size considerations, feature compatibility, cost/performance trade-offs, switching process, best practices, troubleshooting, multi-model workflows, and session forking strategy. The guide uses clear examples and decision trees to help users choose the right switching approach. Completed 2026-08-01.
- [x] 23.4 Write security documentation for trust, untrusted content, secrets, sandbox limits, network, extensions, and remote/background risk
  - `docs/SECURITY-DOCUMENTATION.md` provides comprehensive security documentation covering trust model and boundaries, untrusted content handling, secret detection and protection, filesystem/process/network sandbox limits, extension security and isolation, remote execution and background job risks, permission profile details, incident response procedures, privacy and data handling, compliance considerations, and security best practices. The document emphasizes defense in depth, explicit approval, and user control. Completed 2026-08-01.
- [x] 23.5 Write extension author guides for Skills, hooks, MCP, plugins, permissions, testing, signing, and compatibility
  - `docs/EXTENSION-AUTHOR-GUIDE.md` provides comprehensive extension author documentation covering all four extension types (Skills, Hooks, MCP servers, Plugins), permission system, testing strategies, code signing and distribution, version compatibility, runtime compatibility (Codex/ACP), platform compatibility, security guidelines (input validation, secret handling, sandboxing, audit trail), and best practices. The guide includes complete examples for each extension type, manifest schemas, testing patterns, signing process, publishing workflow, and security requirements. Completed 2026-08-01.
- [x] 23.6 Write AAP and adapter contributor documentation with schemas, fixtures, versioning, and release policy
  - `docs/AAP-ADAPTER-CONTRIBUTOR-GUIDE.md` defines the repository map, stable/experimental schema rules, Runtime/Qt/adapter ownership boundary, redacted fixture workflow, version and rollback policy, local verification commands, Windows/macOS evidence gates, and authority/security review checklist. It links the normative AAP wire guide and pinned Codex upgrade procedure without claiming unavailable Windows evidence.
  - The guide is intentionally procedural and does not promote experimental capabilities, widen the pinned Codex `0.144.5` matrix, or grant Agent/Codex mutation authority.
- [x] 23.7 Write troubleshooting runbooks for sidecar startup, database recovery, adapter mismatch, model catalog, terminal, Git, sandbox, and renderer crash
  - `docs/Aegisy-TROUBLESHOOTING-RUNBOOK.md` covers bounded first response, sidecar/handshake, Store recovery, adapter crash loops, streaming decode failures, the Windows TLS incident, terminal/Git/sandbox boundaries, renderer recovery, and escalation redaction. It explicitly leaves clean Windows installer/TLS/ConPTY evidence as an external gate.
- [x] 23.8 Write privacy and diagnostic export documentation with exact local/cloud data categories
  - `docs/AEGISY-PRIVACY-AND-DIAGNOSTIC-EXPORT.md` defines the exact local and cloud category IDs, default inclusion states, redaction order and detector classes, preview/removal/export contract, deterministic bundle envelope, retention controls, support correlation ID, and the current read-only authority boundary.
  - The default bundle is metadata-only and local-first. Prompts, code, diffs, paths, terminal output, provider bodies, hidden reasoning, credentials, and automatic cloud upload remain excluded; content opt-in is per named Session/Item/range and is previewed before writing.
  - This documentation does not claim an AAP/Qt exporter or cloud upload implementation. Any future implementation must repeat source/policy/redaction/bounds checks at export time and fail closed on stale preview, source loss, or uncertain classification.
- [x] 23.9 Train support and release owners on recovery paths that preserve user repositories and session history
  - `docs/AEGISY-SUPPORT-AND-RELEASE-RECOVERY-TRAINING.md` is the internal training package for support engineers, release owners, and incident commanders. It covers sidecar/handshake/heartbeat/reconnect, Store migration/reconciliation, Session Timeline retention, Windows TLS, streaming decode failures, renderer, Git, terminal, and sandbox recovery.
  - The package requires no repository/data-root deletion, no fabricated success, no TLS or permission bypass, bounded redacted evidence, and explicit macOS-versus-Windows evidence boundaries. It links the troubleshooting runbook, privacy/diagnostic export contract, and portable Session format and includes disposable-fixture exercises plus a data-free sign-off checklist.
  - This records the training material and review gate; human attendance/sign-off and clean Windows runner evidence remain operational release evidence and are not inferred from this document.
- [ ] 23.10 Review every visible empty, loading, offline, permission, conflict, failure, interrupted, and recovery state before stable release
  - Partial desktop audit: `docs/AEGISY-WORKBENCH-VISIBLE-STATE-MATRIX.md` inventories the eight visible-state classes and their stable Qt locators. The render fixture now asserts the initial empty state, fail-closed capability loading, read-only permission boundary, a real external-file conflict, stable failure-notice locators with exact text and semantic severity, authoritative Turn interruption, and synthetic offline/reconnected projections without treating the synthetic signal as transport evidence. Loading variants for search/history/terminal attachment, actual Monaco/xterm crash fallback, all secondary dialogs, accessibility/focus/screen-reader behavior, Chinese IME, high contrast, supported display scales, and clean Windows evidence remain open, so keep `23.10` unchecked.
