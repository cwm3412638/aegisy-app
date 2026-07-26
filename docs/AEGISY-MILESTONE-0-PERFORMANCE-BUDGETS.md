# Aegisy Milestone 0 Performance Budgets

Status: predeclared go/no-go budgets for the Aegisy Coding desktop spike.
These are acceptance thresholds, not measured results. OpenSpec `2.7` owns the
signed macOS and Windows measurements and must publish raw summary evidence before
a release claim.

## Measurement Contract

### Reference machines

Measurements require both of these clean, release-representative classes:

- macOS: the oldest supported Apple Silicon macOS release, 8 GiB RAM, four or
  more performance/efficiency cores, APFS, no developer Qt/Node/Rust paths;
- Windows: the oldest supported Windows x64 release, 8 GiB RAM, four logical
  cores, NTFS, 150% display scaling, Defender enabled, no developer Qt/OpenSSL
  directories on `PATH`.

The final supported OS/CPU matrix remains owned by OpenSpec `1.7`; measurements
must record the exact OS build, architecture, CPU, RAM, filesystem, display scale,
package hash, and power mode. A faster developer machine cannot replace either
reference class.

### Fixtures and runs

- Use signed/installable Release builds with diagnostics at normal production
  level. Debug, ad-hoc component, and test-harness numbers are not release evidence.
- Run at least 20 measured iterations after two unmeasured warm-ups. Report median,
  p95, maximum, and failure count; every threshold below is a p95 unless stated.
- Cold startup follows reboot, clears only documented OS file cache where allowed,
  and starts from no Aegisy process. Warm startup follows one clean exit.
- The standard repository fixture contains 5,000 eligible files, 8 MiB indexed
  UTF-8 source, 20,000 symbols, 10,000 dependency edges, 2,000 Timeline Items,
  one 1 MiB text file, and one terminal stream of 10 MiB. Ignored dependency/build
  trees are present and must remain excluded.
- Network/provider time is excluded from local UI/runtime budgets. Offline state is
  measured explicitly; a network timeout cannot make local startup unbounded.
- Sample instrumentation must use monotonic clocks. Memory is the complete Aegisy
  process tree resident set after a 60-second idle settle. CPU is process-tree CPU
  time divided by wall time and available logical cores, reported with raw CPU time.
- A crash/restart pass also requires state-integrity assertions. Fast recovery that
  loses, duplicates, fabricates, or silently enables state is a failure.

## Budgets

| Dimension | Milestone 0 budget | Required evidence |
| --- | --- | --- |
| Compressed signed installer growth | no more than `+150 MiB` per platform versus the same-version legacy client built with the same compiler/Qt/update settings | exact old/new package bytes and SHA-256; component-attributed delta |
| Installed application growth | no more than `+400 MiB` per platform versus the matched legacy installation | recursive allocated bytes after install; WebEngine/local assets/sidecar attribution |
| Legacy shell cold startup | login or authenticated legacy navigation interactive within `3.0 s`; Workbench failure may add no more than `200 ms` to that path | launch-to-first-interactive timestamps with Workbench healthy, missing, and corrupt |
| Legacy shell warm startup | interactive within `1.5 s` | 20-run warm distribution |
| Workbench cold readiness | shell visible within the legacy budget and local Workbench ready/read-only recovery within `5.0 s` | Qt shell, web renderer, sidecar handshake, and first confirmed Session timestamps |
| Workbench warm readiness | local Workbench ready/read-only recovery within `2.5 s` | same phase breakdown |
| Empty Workbench idle memory | complete process-tree RSS increase no more than `350 MiB` over the legacy client after 60 seconds | host, WebEngine processes, sidecar, and local gateway breakdown |
| Standard active memory | complete process tree no more than `900 MiB` after opening the standard repository, 2,000 Items, Monaco, terminal, and Git view | peak and settled RSS; renderer restart before/after comparison |
| Editor open | 1 MiB UTF-8 file first editable paint within `1.0 s`; 5 MiB file either opens within `2.5 s` or enters the explicit bounded large-file fallback within `1.0 s` | monotonic request/read/model/paint phases and selected renderer/fallback |
| Editor interaction | key-to-paint latency at most `50 ms`; selection and scroll frame stall at most `100 ms` | at least 1,000 sampled interactions including Chinese IME composition |
| User save | 1 MiB atomic save acknowledgement within `1.5 s` with revision/watch reconciliation complete | request-to-authoritative revision; conflict/stale paths measured separately |
| Terminal input | local PTY echo latency at most `50 ms` while idle and `100 ms` during sustained output | 1,000 input samples with Unicode and resize activity |
| Terminal throughput | ingest and render a 10 MiB ANSI/Unicode fixture at at least `5 MiB/s`, with no UI stall over `250 ms` and no byte loss before the documented 1 MiB retained-tail boundary | producer bytes, retained/omitted offsets, wall time, max UI stall |
| Initial repository index | standard 5,000-file/8 MiB fixture completes within `15 s`; process-tree average CPU no more than two fully utilized logical cores and UI p95 interaction remains `<=100 ms` | wall/CPU time, parsed/reused/excluded counts, UI latency during index |
| Incremental index | one changed eligible file is reflected within `1.0 s`; 100 changed files within `3.0 s` | watch-to-complete times and unchanged-file reuse count |
| Renderer crash recovery | last confirmed editor/timeline state visible or explicitly unavailable within `3.0 s`; terminal output is reattached or marked unverified, never inferred exited | injected crash, generation IDs, restored models/tabs/Session/terminal state |
| Sidecar crash recovery | failure/Unknown visible within `2.0 s`; bounded replacement handshake plus Session recovery within `8.0 s` when retry is allowed | health transition, generation, fixed watermark/snapshot activation, no duplicate Turn |
| Full app restart recovery | shell interactive within cold budget and a 2,000-Item Session selected/revalidated within `10.0 s` | Store open/migration, Session paging, renderer, and sidecar phases |

## Hard Correctness Gates

The following fail Milestone 0 regardless of timing:

- login, legacy gateway/configuration, update, export, or logout is blocked by a
  Workbench/renderer/sidecar/Store failure;
- UI, PTY, sidecar, gateway, renderer, or language-server processes remain orphaned
  after the measured shutdown/recovery window;
- confirmed Timeline/editor/terminal state is lost, duplicated, fabricated, or
  accepted from an old process generation;
- a measurement disables TLS, antivirus, signing, CSP, sandbox, path, permission,
  approval, or read-only gates;
- ignored/sensitive/symlinked files enter indexing or benchmark output; or
- credentials, prompts, source, diffs, terminal content, provider bodies, full
  paths, PIDs, or raw stderr enter the published performance evidence.

## Decision And Regression Policy

- OpenSpec `2.7` must record actual signed-package results for both reference
  platforms. A missing metric is a failed gate, not zero cost.
- Any p95 budget miss blocks the embedded-WebEngine go/no-go ADR until the design is
  optimized or a product/security owner approves a written, time-bounded exception
  with user impact and rollback. Correctness gates cannot be waived.
- After Milestone 0, CI or release runners must compare the same fixture against the
  last approved baseline. A regression over `10%` in any latency/throughput/memory
  metric requires review even when still below the absolute ceiling.
- Measurements, exceptions, and regressions are metadata-only release evidence;
  fixture content and user data are never uploaded.
