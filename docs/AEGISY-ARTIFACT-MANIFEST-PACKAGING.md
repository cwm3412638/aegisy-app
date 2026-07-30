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

- package the pinned adapter as a reviewed artifact and generate the manifest
  after deployment/copying and before signing;
- add a non-downgradable packaged-build identity that requires the manifest while
  preserving manifest-free developer builds;
- close the remaining verification-to-process-creation replacement window with a
  reviewed file-handle/platform-signature and installed-directory permission
  boundary; repeated path hashing narrows but does not eliminate that race;
- bind the manifest's runtime/adapter versions to updater compatibility checks,
  rejecting an update that would leave an incompatible pair;
- add signed macOS and Windows release evidence. This foundation does not claim
  signatures, notarization, Authenticode, or Windows verification.
