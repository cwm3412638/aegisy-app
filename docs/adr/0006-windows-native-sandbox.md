# ADR 0006: Native Windows Sandbox Selection

- Status: Deferred
- Accountable owner: Product Security
- Consulted owners: Windows Runtime, Release Engineering, Enterprise Security
- Due gates: OpenSpec `18.10`; write-capable Windows release blocked by `18.12`

## Context

ConPTY and Job Objects provide terminal/process lifecycle control, not a complete
Agent sandbox. A write-capable Agent needs filesystem, process, registry, credential,
network, child-process, and cleanup isolation without requiring administrator rights.
No candidate combination has current clean-machine security evidence.

## Decision State

Do not select or simulate a Windows sandbox yet. Keep Agent/Codex read-only on
Windows. User terminals remain an explicit user operation and do not grant Agent
execution authority.

The selection study must compare at least restricted tokens, AppContainer or
low-integrity isolation, Job Objects, filesystem ACL/capability projection, network
policy enforcement, process mitigation policies, and a brokered file-operation model.
Windows Sandbox/VM features that require administrator or optional OS components
cannot be the only supported path.

## Acceptance Evidence

- Clean Windows 10/11 x64 machines with standard-user installation and no developer
  toolchain on PATH.
- Denial of protected paths, credentials, registry, named objects, process escape,
  network hosts, symlink/junction/reparse escapes, wrapper commands, and child-process
  orphaning under adversarial races.
- Exact approved workspace writes, cancellation, crash recovery, installer/update,
  antivirus, long-path, Unicode, and 125%/150% scaling behavior.
- Content-free audit and recovery evidence with no PID adoption or boolean approval
  shortcut.

## Consequences

Compilation or a macOS fixture is not Windows sandbox evidence. Job Object cleanup,
ConPTY behavior, Defender acceptance, or an installer that starts successfully cannot
individually close this ADR. Windows Agent mutation remains unadvertised until the
selected design and `18.12` report pass.
