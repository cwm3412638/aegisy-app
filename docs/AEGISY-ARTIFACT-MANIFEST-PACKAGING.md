# Artifact Manifest Packaging Contract

The local verifier consumes `aegisy-artifact-manifest/0.1`. Release packaging
must generate that manifest only after the final bundled sidecar and pinned
adapter files are present. `cmake/generate_artifact_manifest.cmake` is the
deterministic generator foundation:

```text
cmake -DBASE_DIR=<bundle-root> \
  -DOUTPUT=<bundle-root>/aegisy-agentd.manifest.json \
  -DRUNTIME_PATH=<bundle-root>/aegisy-agentd \
  -DRUNTIME_ID=aegisy-agentd -DRUNTIME_VERSION=<runtime-version> \
  -DADAPTER_PATH=<bundle-root>/codex \
  -DADAPTER_ID=codex-app-server \
  -DADAPTER_VERSION="codex-cli 0.144.5" \
  -P cmake/generate_artifact_manifest.cmake
```

The generator accepts only explicit regular files inside `BASE_DIR`, records
relative paths, computes SHA-256 from the final bytes, and writes stable JSON
without timestamps or machine paths. It performs no network access, execution,
signing, or environment discovery. The CTest
`artifact_manifest_generation` fixture runs it twice and rejects output drift
and paths outside the bundle.

## Signed Update Artifact Set Compatibility

`include/update_signing_key_ring.h`, `src/update_signing_key_ring.cpp`,
`include/update_artifact_set.h`, and `src/update_artifact_set.cpp` define the
platform-neutral production trust path. A compile-time
`aegisy-update-signing-trust-anchor/0.1` authenticates an exact-generation
`aegisy-update-signing-key-ring/0.1`, and that authority verifies
`aegisy-update-artifact-set/0.2`. The result is a local compatibility decision
only. It performs no network request, download, installer launch,
update-framework callback, Key Ring persistence, high-water persistence, or
policy intersection. The checked-in production Root settings are intentionally
empty until release-owned key configuration and publication are reviewed.

Bootstrap accepts generation one only when the exact embedded Root signs a Ring
containing that same Root with both `key-ring` and `artifact-set` usage. Rotation
must advance exactly one generation, retain every prior Key ID and public key,
preserve lineage and usage, never widen prior validity, and never reverse
revocation. Ring signature time is monotonic. Each authority retains the first
generation and signing time at which a key was admitted, so neither a Ring nor an
Artifact Set can claim a signature from before that key entered the trusted
lineage. `signed_at_ms` is signer-controlled metadata, not trusted timestamp
evidence. First-time bootstrap and rotation therefore require the signer to be
active both at the declared signing time and at local verification time. An exact
already accepted envelope remains idempotently replayable after expiry, but an
offline client cannot first admit it after expiry without a future independent
witness or authenticated checkpoint. Explicit revocation remains effective.
Generations from two onward bind the complete admission history into the authority
identity, so two histories that converge on the same later Ring remain distinct.

The signed envelope binds all of the following:

- positive release sequence, signing and exclusive-expiry times, signing Key ID,
  payload identity, and one of `internal`, `preview`, `beta`, or `stable`;
- application version, size, SHA-256, and exact `macos/arm64` or
  `windows/x86_64` target;
- canonical HTTPS installer URL, ASCII basename, positive size up to 2 GiB,
  lowercase SHA-256, and canonical 64-byte Sparkle Ed25519 signature;
- target `aegisy-artifact-manifest/0.1` SHA-256 plus exact Runtime and adapter
  identities and versions;
- one to 64 strictly increasing, lower-sequence source artifact sets containing
  the complete application version/target/size/SHA-256 and manifest identity
  from which the target may be installed; and
- an outer Ed25519 signature over a fixed ordered line payload. The outer
  signature is excluded from that payload, while every compatibility field is
  included.

Verification accepts at most 256 KiB of raw JSON. It uses the repository lossless
parser so duplicate decoded keys, fractional or non-JSON-safe numbers, invalid
UTF-8, excessive nesting/nodes, unknown fields, and malformed values fail closed
before signature verification. Installer URLs forbid redirects-by-URL syntax,
userinfo, query, fragment, non-443 explicit ports, percent-encoded paths, empty or
dot path segments, Unicode/control ambiguity, and a basename mismatch. Windows
also rejects reserved device basenames and requires an exact lowercase `.exe`;
macOS requires an exact lowercase `.zip`.

Production evaluation requires an opaque `InstalledArtifactSetAuthority`; callers
cannot construct the installed tuple or select verification paths. The only production
factory, `verifyCurrentInstallationAuthority`, derives the layout from
`QCoreApplication::applicationFilePath()`: Windows uses the directory containing the
exact `AegisyClient.exe`, while macOS requires the exact internal
`Contents/MacOS/AegisyClient` layout and uses that `MacOS` directory because the current
package places the sidecar there. The outer macOS bundle basename may be changed by the
user and is not treated as package-signature authority. The exact adjacent files are
`aegisy-update-artifact-set.json`, `aegisy-agentd.manifest.json`, and
`aegisy-agentd[.exe]`. The receipt and Manifest must be ordinary, non-reparse,
single-link files.

The factory verifies the receipt signature through the current Key Ring authority
and target, verifies the Runtime/adapter Manifest bytes, identity, version,
ordinary-file/link policy, and SHA-256 values, and derives an authority identity
from the receipt, complete installed tuple, trust anchor, Ring and signer identities,
canonical layout, directory identities, and the current
application-path target plus receipt, Manifest, Runtime, and adapter canonical paths,
native file identities, sizes, and SHA-256 values. Each hashed file must have the same
native identity when inspected by path before open, through the actual opened read
handle, and by path after the read. Runtime and adapter must also be different canonical
paths and different native files. The application and directories are observed again
after artifact verification. `verifyCandidate` derives and revalidates that same current
layout before every decision, so even an exact-byte replacement with a different file
identity invalidates a cached authority.
The legacy raw-key, scalar tuple, arbitrary-root, and Artifact Set `0.1` entry
points are compiled only into dedicated compatibility tests. A separate
production-shaped child-process target is compiled without either testing macro.
It copies the test image into the fixed `AegisyClient` layout and proves embedded
Root bootstrap, generation-two rotation, historical installed-receipt authority,
rotated-key candidate evaluation, fixed-false download/install authority, and
production rejection of an Artifact Set `0.1` envelope.

The verifier also requires the selected channel and an accepted release-sequence
high-water value. Its evaluation identity binds the candidate identity, installed-set
and authority identities, trust anchor, Ring generation/identity/authority, exact
receipt and candidate signer, channel, high-water value, and evaluation time so a
cached result cannot be reused after any of those inputs changes.
A result may set only `candidateCompatible=true`; `downloadAuthorized` and
`installAuthorized` remain
false for every result. The ordinary integer high-water argument is not proof of
durable, integrity-checked, anti-deletion storage. The fixed-layout factory proves
that the target currently named by `applicationFilePath()` and each opened adjacent
artifact were byte- and identity-stable across the bounded observations. It does not
prove that this path still names the image loaded when the process started, keep the
verified handles through later spawn/install, verify macOS code signing/notarization,
verify Windows Authenticode or an outer installer signature. It therefore is not yet proof that
the current process belongs to the signed Aegisy package described by the receipt.
The current authority does bind Key IDs, validity/revocation, monotonic rotation
lineage, admission history, and the latest Ring identity, but the application does
not persist or fetch that lineage and no release Root is configured. It therefore
does not yet provide cross-restart rollback protection or production trust. A
production recovery record must bind at least the
release sequence, exact artifact-set identity, and update phase. Exact-identity retries
may then resume idempotently while a same-sequence different identity fails closed;
advancing an unqualified scalar before or after download cannot provide both crash
recovery and replay resistance.

### Update Progress Continuity Foundation

`update-progress-record/0.1` is an internal, single-writer continuity foundation for
that future recovery record. It stores an exact release sequence, artifact-set
identity, ordered phase, revision, timestamp, prior-record identity, and current
record identity. The phases are `candidate-evaluated`, `download-started`,
`download-verified`, `install-started`, and `installation-observed`. Same-sequence
identity conflicts, skipped phases, non-monotonic revisions/time, incomplete-release
replacement, rollback below an externally supplied floor, malformed JSON, duplicate
keys, links, extra hard links, and Unix owner-execute, group/other, or
setuid/setgid/sticky modes fail closed.
Writes use `QSaveFile`, checked private Unix permissions, and post-commit reread;
exact uncertain retries may return the same record. Every download/install/rollback
authority field is fixed false.

The Store is not connected to either updater and is deliberately not an anti-deletion
anchor. Deleting both the record and its external high-water evidence permits a fresh
record, and the current API depends on a trusted caller-supplied record identity/floor
to detect deletion or rollback. A local `QLockFile` gate serializes writers and
expected-record-identity comparison provides CAS under that lock. The Store still
lacks a reviewed secure-storage anchor, a transaction spanning that anchor, updater/
framework state, and signed package identity, plus clean Windows multi-process and
crash-injection evidence. It must not be used to authorize network, download,
install, rollback, or resume operations.

Framework integration also remains open. Sparkle 2.9.4 can synchronously reject a
newly discovered candidate through `SPUUpdaterDelegate`, but its resume path skips
that veto and therefore needs a separate pre-install recheck. WinSparkle 0.9.3 has
no candidate object or download-before veto; its callbacks occur too late to meet
this boundary. Windows must use an audited Aegisy-owned update flow or a maintained
reviewed fork before local candidate rejection can be claimed. Server-side feed
filtering, custom appcast fields, and download-after callbacks are not substitutes
for the local gate. The `0.2` contract binds only the full installer. The currently
generated Sparkle deltas must be disabled before integration or each delta must gain
an equally strict signed source/target, URL, size, hash, signature, and resume-time
compatibility binding.

## Release Gaps

The current macOS and Windows packaging scripts bundle `aegisy-agentd` but do
not bundle a pinned Codex adapter executable. They must not generate a manifest
until that adapter packaging decision, source, version, and license inventory
are fixed by the release process. The generator is therefore intentionally not
called from either script yet.

Rust Runtime consumes the same adjacent manifest when it exists. That path takes
priority over `AEGISY_CODEX_PATH`, validates the exact Runtime and pinned adapter
versions/files, requires one-link artifact identities, requires the exact `.exe`
adapter path on Windows, fixes the manifest and opened-file identities for a startup
attempt, and revalidates before the version probe and App Server spawn. Hashing uses
a bounded heap buffer. A malformed present manifest fails closed. Manifest absence
still enables developer discovery because current packages do not yet bundle the
reviewed adapter or generate the contract.

The following work remains required for OpenSpec 22.5:

- package the pinned adapter as a reviewed artifact, finish signing the nested
  Runtime/adapter binaries, generate the manifest from those final bytes, then include
  it in the outer application/installer seal; macOS nested signing or Windows
  Authenticode after manifest generation would invalidate the recorded hashes;
- add a non-downgradable packaged-build identity that requires the manifest while
  preserving manifest-free developer builds;
- close the remaining verification-to-process-creation replacement window with a
  reviewed file-handle/platform-signature and installed-directory permission
  boundary; repeated path hashing narrows but does not eliminate that race;
- integrate the signed artifact-set decision into a reviewed updater while
  rechecking resumed downloads and preserving all policy/authority gates;
- configure the release Root, persist a validated monotonic Key Ring/high-water
  identity outside caller control, and fetch Root-authenticated sequential Ring
  updates without permitting rollback, gaps, or local deletion to reset authority;
- promote the current update-progress continuity Store only after it has a reviewed
  secure anti-deletion anchor, clean-platform/crash validation of its writer lock,
  and one recovery transaction binding that anchor, the progress record, updater/
  framework state, and signed package identity; then integrate its exact sequence,
  artifact-set identity, and phase transitions into the updater;
- bind the now fixed current-installation layout and application image hash to a
  verified, non-downgradable signed application package/manifest authority, and bind
  the actual loaded process image rather than only the current application path target;
- define an independently authenticated offline recovery/checkpoint path before a
  client may first admit a Ring whose signer is no longer currently active; a
  self-declared `signed_at_ms` alone must never provide that authority;
- select an audited Windows pre-download implementation instead of treating
  WinSparkle's post-download callbacks as compatibility authority;
- disable Sparkle deltas or bind and revalidate every delta as a complete signed
  source-to-target artifact transition;
- add signed macOS and Windows release evidence. This foundation does not claim
  signatures, notarization, Authenticode, or Windows verification.
