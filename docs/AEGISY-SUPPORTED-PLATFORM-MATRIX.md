# Aegisy Coding Supported Platform Matrix

Status: approved baseline policy for OpenSpec `1.7`

Last reviewed: 2026-07-26

## Purpose

This matrix separates four facts that must not be collapsed into one support
claim:

1. an upstream dependency can run on a platform;
2. this repository can build a target;
3. automated or clean-machine evidence has exercised the target; and
4. Aegisy may promise that target to users.

A platform is publicly supported only when all four layers pass for the exact
signed artifact. Compilation, a developer-machine launch, a Qt support statement,
or a screenshot is not release evidence by itself.

## Support Levels

| Level | Meaning |
| --- | --- |
| Upstream-capable | The pinned Qt line documents the operating system and CPU combination. |
| Build target | Repository configuration and packaging select the combination. |
| Evidence target | CI or a named clean-machine run exercises the required matrix. |
| Release-supported | Signed installer, runtime, recovery, accessibility, and update gates all pass. |
| Unsupported | The combination is rejected or receives no compatibility promise. |

## Operating System And CPU Matrix

| Platform | Upstream Qt 6.8 capability | Repository build/package target | Current evidence | Aegisy release status |
| --- | --- | --- | --- | --- |
| macOS 26, Apple silicon (`arm64`) | Qt 6.8 supports macOS 12+ on `arm64` | `CMAKE_OSX_DEPLOYMENT_TARGET` and `package-macos.sh` currently default to `26.0`; package architecture is detected with `lipo` | Current macOS 26.5.2 development host uses Qt 6.11.1; the built GUI artifact is arm64 and passes the recorded local tests | Internal development baseline only. A pinned Qt release line, Developer ID signed, notarized, clean-machine package and the full matrix below are still required before public support. |
| macOS 12-15, Apple silicon | Upstream-capable | The current package target `26.0` cannot run here | No repository release evidence | Unsupported by the current package. Lowering the deployment target requires a separate build, signed-package, dependency, WebEngine, updater, and runtime test gate. |
| macOS 12+, Intel (`x86_64`) | Upstream-capable | CMake can target it, but no maintained package or universal-build configuration exists | No current clean-host or signed-package evidence | Unsupported. A native `x86_64` or verified universal package is required before promotion. |
| Windows 10 1809+ x64 | Qt 6.8 and ConPTY technical floor | `package-windows.bat` selects Visual Studio 2022 `x64`; CI pins Qt 6.8.3 and x64 OpenSSL | `windows-2022` CI builds selected Qt/Rust/package gates; clean Windows 10 installer, TLS, ConPTY, Git, high-DPI, IME, and update evidence is incomplete | Preview candidate only, not release-supported. Runtime terminal code rejects pre-1809, but the installer does not yet declare `MinVersion`; release packaging must enforce the same floor. |
| Windows 11 x64 | Qt 6.8 upstream target | Same x64 package as above | CI build evidence exists; clean-user-machine acceptance is incomplete | Preview candidate only, not release-supported. |
| Windows on ARM64 / ARM64EC | Qt 6.8 lists ARM64 but not ARM64EC | Repository packaging, Rust target, OpenSSL, updater, and tests are x64-only | None | Unsupported. x64 emulation does not count as native ARM64 support. |
| Linux, mobile, web, remote browser | Some dependencies may support these targets | No Aegisy desktop package target | None | Unsupported. Remote Workbench is governed separately by the feature policy and requires its own OpenSpec/security gate. |

Upstream reference: [Qt 6.8 Supported Platforms](https://doc.qt.io/qt-6.8/supported-platforms.html).
The repository target is intentionally narrower than Qt's capability.

## Filesystem Matrix

| Area | Baseline | Required evidence | Unsupported or gated behavior |
| --- | --- | --- | --- |
| macOS project volume | Local APFS, default case-insensitive mode; case-sensitive APFS is a required adversarial fixture | Canonical root identity, Unicode normalization, case collision, symlink denial, atomic save, executable mode, watcher conflict, low-space, checkpoint, and crash recovery | Network shares, cloud-synchronized folders, removable media, and filesystems without required atomic/link semantics are unsupported until separately tested. |
| Windows project volume | Local NTFS | Case-insensitive canonicalization, Unicode, junction/reparse denial, ACL/locking conflict, atomic replace, long path, Defender interaction, ConPTY, and Job Object cleanup | FAT/exFAT, ReFS, SMB, OneDrive Files On-Demand, substituted drives, and WSL/Linux filesystems are unsupported until an explicit matrix is added. |
| Project roots | Existing local directories with stable filesystem identity | Move/relink, additional read-only/read-write roots, nested repository, unavailable root, and restart evidence | A symlink final root, sensitive location, project-contained executable/tool override, or caller-selected arbitrary host path is denied. |
| Metadata and recovery stores | Platform application-data location, disjoint from project roots, private to the user | SQLite WAL recovery, permissions/ACLs, low-space, backup, tamper, deletion, and conservative Blob GC | Storing the Workbench database, checkpoint store, or credential material inside a project is unsupported. |
| Git metadata | In-root `.git` for mutation foundations; read-only nested repository queries are separately scoped | Worktree/common-dir identity, index/ref races, long paths, case collisions, hooks/filter denial, crash recovery | External gitdirs, parent-repository mutation, linked-worktree mutation, LFS/filter execution, and arbitrary remote credentials remain gated unless a task explicitly authorizes them. |

`MAX_PATH` success on one host is not long-path support. The current Windows
application manifest has no `longPathAware` declaration. Windows release evidence
must first add and verify the intended application/OS policy, then exercise project
paths above 260 UTF-16 code units across build, file, Git, terminal, and recovery
paths.

## Shell And Terminal Matrix

| Platform | User terminal discovery order | Supported contract | Not implied |
| --- | --- | --- | --- |
| macOS | Absolute user shell when policy-safe, then `/bin/zsh`, then `/bin/sh` | zsh (`-f -i`), bash (`--noprofile --norc -i`), and fallback POSIX sh; interactive PTY, UTF-8/ANSI, resize, bounded capture, foreground signals, escalation, and process-group cleanup | Fish and other shells receive only `-i` and are unsupported until clean-startup fixtures prove their user configuration cannot widen authority. A project-contained shell is rejected. |
| Windows | PowerShell 7, Windows PowerShell, then absolute system `ComSpec`/`cmd.exe` | ConPTY on Windows 10 1809+, UTF-8 channel, profiles/AutoRun disabled, Ctrl+C, resize, Job Object tree cleanup | WSL, Git Bash, Cygwin/MSYS2, third-party terminals, PowerShell profiles, and project-contained shells are unsupported. |

The table applies to explicit user terminals. It grants no Agent command or
background execution authority.

## Git Version Policy

- Supported product Git is the external system Git `2.31.0` through the newest
  version that passes the release matrix. The minimum is a declared test floor, not
  evidence that every vendor-patched build behaves identically. It covers the
  current command surface's `rev-parse --path-format` requirement; commands such as
  `switch` have an older floor and do not lower the product-wide minimum.
- The current macOS development evidence uses Apple Git `2.50.1` (`Apple Git-155`).
- Windows evidence must record the exact Git for Windows version from the clean
  runner. CI-provided Git alone does not prove the installed application contract.
- Runtime discovery must resolve an absolute executable outside the project, parse
  a bounded machine-readable version, and fail closed below the minimum or when the
  version cannot be classified. Until that enforcement exists, Git product support
  remains provisional even when individual read-only fixtures pass.
- Release testing covers the minimum version and the current pinned release version.
  A major/minor upgrade requires the read-only query, checkpoint, worktree, staging,
  commit, workflow, cancellation, and recovery suites before promotion.

## Display, Input, And Accessibility Matrix

| Platform | Required scales | Required input and accessibility evidence |
| --- | --- | --- |
| macOS | 1x and 2x backing scale; standard and Retina multi-display movement | Chinese Pinyin IME composition, clipboard, drag/drop, full keyboard traversal, visible focus, VoiceOver, reduced motion, light/dark appearance, and renderer fallback. |
| Windows | 100%, 125%, 150%, and 200%; per-monitor movement between unlike scales | Microsoft Pinyin IME composition, clipboard, drag/drop, full keyboard traversal, Narrator, high contrast, text scaling, light/dark appearance, and renderer fallback. |

Every required scale covers login, navigation rail, Chat/Work switch, Session list,
Timeline/composer, menus and combo boxes, Monaco, xterm, Changes, Git, dialogs,
failure/recovery banners, and updater surfaces. Text clipping, overlapping controls,
lost focus, blank WebEngine content, incorrect pointer mapping, or an inaccessible
approval is a release failure.

## Evidence And Promotion Gates

A row can move to `Release-supported` only when the exact artifact has:

1. a reproducible Release build with pinned Qt, compiler, OpenSSL, sidecar, adapter,
   schemas, local web assets, updater, and artifact manifest;
2. valid platform signing, macOS notarization or Windows Authenticode, and update
   signature verification;
3. a clean standard-user install with no developer Qt/OpenSSL/Rust/Node directories
   on `PATH`;
4. real login/API TLS, sidecar startup, Store migration/recovery, Codex compatibility,
   Monaco/xterm, filesystem, Git, cancellation, crash, update, and uninstall tests;
5. the filesystem, shell, display, IME, accessibility, security, and performance
   matrix above with redacted evidence attached to the release candidate;
6. no open correctness, state-loss, security-boundary, orphan-process, or installer
   blocker; and
7. an explicit release-owner sign-off naming the OS build, CPU, artifact hash,
   dependency versions, known limitations, rollback artifact, and feature channel.

Missing evidence leaves the row unsupported or preview-only. It never inherits a
claim from a nearby OS version, architecture, Qt statement, CI compile, or macOS run.

## Ownership And Change Control

- Desktop Platform owns OS/CPU/display and WebEngine evidence.
- Runtime Integrations owns shell, terminal, Git, sidecar, and adapter evidence.
- Product Security owns filesystem, sandbox, credential, IPC, and hostile-path gates.
- Release Engineering owns clean-machine, signing, installer, updater, and rollback
  evidence.
- Accessibility owns IME, keyboard, screen reader, high contrast, and text-scale
  acceptance.

Any support expansion updates this document, the release checklist, OpenSpec task
and verification evidence, and `PROJECT-MEMORY.md`. A dependency support statement
alone cannot expand the Aegisy matrix.
