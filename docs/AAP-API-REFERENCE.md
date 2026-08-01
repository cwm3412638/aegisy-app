# Aegisy Agent Protocol - API Quick Reference

## Overview

Quick reference for AAP (Aegisy Agent Protocol) methods, events, and common patterns. For complete details, see [AAP-PROTOCOL-GUIDE.md](AAP-PROTOCOL-GUIDE.md).

**Protocol**: JSON-RPC 2.0 over stdio/Unix socket  
**Version**: AAP 0.1  
**Frame Limit**: 4 MiB

## Connection Lifecycle

### 1. Initialize
```json
Request:  runtime/initialize
Response: Capability negotiation
```

### 2. Initialized Notification
```json
Notification: initialized (no params)
```

### 3. Heartbeat (Optional)
```json
Request:  runtime/heartbeat
Response: Connection liveness proof
```

## Core Methods

### Runtime

| Method | Purpose | Capability Required |
|--------|---------|-------------------|
| `runtime/initialize` | Handshake and capability negotiation | None (pre-handshake) |
| `runtime/heartbeat` | Connection liveness check | `runtime.heartbeat.out-of-band` |
| `runtime/health` | Runtime health status | `runtime.health` |
| `runtime/degradations` | Check degraded subsystems | `runtime.degradations` |
| `runtime/restart` | Restart runtime | `runtime.restart` |
| `runtime/shutdown` | Graceful shutdown | `runtime.shutdown` |

### Project

| Method | Purpose | Capability Required |
|--------|---------|-------------------|
| `project/list` | List available projects | `project.list` |
| `project/open` | Open project | `project.open` |
| `project/close` | Close project | `project.close` |
| `project/roots/list` | List project roots | `project.roots.read` |
| `project/roots/add` | Add filesystem root | `project.roots.write` |
| `project/roots/remove` | Remove filesystem root | `project.roots.write` |
| `project/trust/review` | Get trust review | `project.trust.review` |

### Session

| Method | Purpose | Capability Required |
|--------|---------|-------------------|
| `session/create` | Create new session | `session.create` |
| `session/list` | List sessions | `session.list` |
| `session/read` | Read session history | `session.read` |
| `session/resume` | Resume session | `session.resume` |
| `session/fork` | Fork session | `session.fork` |
| `session/archive` | Archive session | `session.archive` |
| `session/delete` | Delete session | `session.delete` |
| `session/recovery/status` | Check recovery status | `session.recovery.status` |

### Turn

| Method | Purpose | Capability Required |
|--------|---------|-------------------|
| `turn/start` | Start model turn | `turn.start` |
| `turn/cancel` | Cancel active turn | `turn.cancel` |
| `turn/steer` | Steer turn execution | `turn.steer` |

### Timeline

| Method | Purpose | Capability Required |
|--------|---------|-------------------|
| `timeline/sync` | Sync timeline events | `timeline.sync` |
| `timeline/subscribe` | Subscribe to events | `timeline.subscription.fixed-watermark` |
| `timeline/subscription-sync` | Sync subscription | `timeline.subscription.fixed-watermark` |
| `timeline/subscription-snapshot` | Get snapshot | `timeline.subscription.fixed-watermark` |
| `timeline/subscription-activate` | Activate subscription | `timeline.subscription.fixed-watermark` |

### Workspace

| Method | Purpose | Capability Required |
|--------|---------|-------------------|
| `workspace/list` | List directory contents | `workspace.read` |
| `workspace/read` | Read file | `workspace.read` |
| `workspace/metadata` | Get file metadata | `workspace.read` |
| `workspace/search` | Search files | `workspace.search` |
| `workspace/watch` | Watch for changes | `workspace.watch` |
| `workspace/edit/preview` | Preview edit | `workspace.edit.preview` |
| `workspace/edit/apply` | Apply edit | `workspace.edit.apply` |
| `workspace/edit/artifact/read` | Read edit artifact | `workspace.edit.preview` |

### Terminal

| Method | Purpose | Capability Required |
|--------|---------|-------------------|
| `terminal/list` | List terminals | `terminal.list` |
| `terminal/open` | Open terminal | `terminal.open` |
| `terminal/attach` | Attach to terminal | `terminal.attach` |
| `terminal/input` | Send input | `terminal.input` |
| `terminal/resize` | Resize terminal | `terminal.resize` |
| `terminal/stop-user` | Stop terminal | `terminal.stop-user` |
| `terminal/restart-user` | Restart terminal | `terminal.restart-user` |
| `terminal/remove-user` | Remove terminal | `terminal.remove-user` |
| `terminal/close` | Close terminal | `terminal.close` |

### Git

| Method | Purpose | Capability Required |
|--------|---------|-------------------|
| `git/status` | Get Git status | `git.read` |
| `git/overview` | Get repository overview | `git.read` |
| `git/log` | Get commit log | `git.read` |
| `git/diff` | Get diff | `git.read` |
| `git/context` | Get Git context | `git.read` |
| `git/worktree/list` | List worktrees | `git.worktree.read` |

### Model

| Method | Purpose | Capability Required |
|--------|---------|-------------------|
| `model/catalog` | Get model catalog | `model.catalog.read-only` |
| `model/catalog-cache` | Get catalog cache | `model.catalog.cache.read-only` |
| `model/catalog-refresh-status` | Get refresh status | `model.catalog.refresh.status.read-only` |
| `model/capability-check` | Check capabilities | `model.capability-check.read-only` |
| `model/profile/list` | List profiles | `model.profile.read-only` |
| `model/profile/read` | Read profile | `model.profile.read-only` |

### Context

| Method | Purpose | Capability Required |
|--------|---------|-------------------|
| `context/inspect` | Inspect context | `context.inspect` |
| `context/pinned/list` | List pinned context | `context.pinned.read` |
| `context/pinned/add` | Add pinned context | `context.pinned.write` |
| `context/pinned/remove` | Remove pinned context | `context.pinned.write` |
| `context/pinned/reorder` | Reorder pinned context | `context.pinned.write` |

### Artifact

| Method | Purpose | Capability Required |
|--------|---------|-------------------|
| `artifact/read-command-output` | Read command output | `artifact.read-command-output` |

### Operation

| Method | Purpose | Capability Required |
|--------|---------|-------------------|
| `operation/status` | Get operation status | `operation.status` |
| `operation/probe` | Probe operation | `operation.probe` |
| `operation/reconcile` | Reconcile operation | `operation.reconcile` |

### Mutation Acknowledgement

| Method | Purpose | Capability Required |
|--------|---------|-------------------|
| `session/mutation-acknowledgements` | List acknowledgements | `session.mutation-acknowledgements` |
| `mutation/acknowledgement/consume` | Consume acknowledgement | `mutation.acknowledgement.consume` |

## Events

### Timeline Events

| Event | Description |
|-------|-------------|
| `timeline/event` | Timeline event notification |
| `timeline/subscription-event` | Subscription event |

### Item Events

| Event | Description |
|-------|-------------|
| `item.created` | Item created |
| `item.updated` | Item updated |
| `item.completed` | Item completed |
| `item.failed` | Item failed |

### Turn Events

| Event | Description |
|-------|-------------|
| `turn.started` | Turn started |
| `turn.completed` | Turn completed |
| `turn.failed` | Turn failed |
| `turn.interrupted` | Turn interrupted |

### Workspace Events

| Event | Description |
|-------|-------------|
| `workspace-edit-proposal` | Edit proposal created |
| `workspace-edit-applied` | Edit applied |

## Common Patterns

### Starting a Turn

```typescript
// 1. Create or resume session
const session = await client.request('session/create', {
  project_id: 'proj-123',
  mode: 'work'
});

// 2. Start turn
const turn = await client.request('turn/start', {
  session_id: session.id,
  prompt: 'Implement feature X',
  context: []
});

// 3. Listen for events
client.on('timeline/event', (event) => {
  if (event.type === 'turn.completed') {
    console.log('Turn completed');
  }
});
```

### Applying Workspace Edit

```typescript
// 1. Preview edit
const preview = await client.request('workspace/edit/preview', {
  session_id: 'sess-123',
  project_id: 'proj-123',
  edit: {
    operations: [
      {
        type: 'update',
        path: 'src/main.ts',
        base_hash: 'abc123...',
        content_ref: 'workspace-edit-content:sha256:def456...'
      }
    ]
  }
});

// 2. Show preview to user
showDiff(preview);

// 3. Apply if approved
if (userApproved) {
  const result = await client.request('workspace/edit/apply', {
    session_id: 'sess-123',
    project_id: 'proj-123',
    edit_id: preview.edit_id
  });
}
```

### Terminal Management

```typescript
// 1. Open terminal
const terminal = await client.request('terminal/open', {
  session_id: 'sess-123',
  kind: 'foreground',
  shell: '/bin/zsh'
});

// 2. Attach and read output
const output = await client.request('terminal/attach', {
  session_id: 'sess-123',
  terminal_id: terminal.id,
  offset: 0
});

// 3. Send input
await client.request('terminal/input', {
  session_id: 'sess-123',
  terminal_id: terminal.id,
  data: 'npm test\n'
});

// 4. Stop terminal
await client.request('terminal/stop-user', {
  session_id: 'sess-123',
  terminal_id: terminal.id
});
```

### Timeline Subscription

```typescript
// 1. Subscribe to timeline
const subscription = await client.request('timeline/subscribe', {
  session_id: 'sess-123'
});

// 2. Sync events
const sync = await client.request('timeline/subscription-sync', {
  session_id: 'sess-123',
  subscription_id: subscription.id,
  after: null
});

// 3. Get snapshot if needed
if (sync.snapshot_required) {
  const snapshot = await client.request('timeline/subscription-snapshot', {
    session_id: 'sess-123',
    subscription_id: subscription.id
  });
}

// 4. Activate subscription
await client.request('timeline/subscription-activate', {
  session_id: 'sess-123',
  subscription_id: subscription.id
});

// 5. Listen for events
client.on('timeline/subscription-event', (event) => {
  handleTimelineEvent(event);
});
```

## Error Codes

| Code | Name | Description |
|------|------|-------------|
| -32700 | Parse error | Invalid JSON |
| -32600 | Invalid Request | Invalid JSON-RPC |
| -32601 | Method not found | Unknown method |
| -32602 | Invalid params | Invalid parameters |
| -32603 | Internal error | Internal error |
| -32000 | Server error | Generic server error |
| -32001 | Not initialized | Called before initialized |
| -32002 | Capability not negotiated | Missing capability |
| -32003 | Protocol incompatible | Version mismatch |
| -32004 | Overload | Request queue full |
| -32148 | Timeline retention gap | Event not available |
| -32149 | Session attempt exists | Duplicate subscription |
| -32150 | Subscription ID reused | Invalid subscription |
| -32151 | Subscription not active | Not activated |
| -32152 | Subscription required | Turn needs subscription |

## Capability Groups

### Read-Only
- `permission.read-only`
- `runtime.health`
- `runtime.preview`
- `project.list`
- `session.list`
- `session.read`
- `workspace.read`
- `git.read`
- `model.catalog.read-only`

### Workspace Write
- All Read-Only capabilities
- `workspace.edit.preview`
- `workspace.edit.apply`
- `workspace.write`

### Developer
- All Workspace Write capabilities
- `terminal.open`
- `terminal.input`
- `terminal.stop-user`
- `git.write`

### Full Access
- All Developer capabilities
- Network access (future)
- System operations (future)

## Schema Versions

All requests and responses include `schema_version` field:

- `runtime-heartbeat-request/0.1`
- `runtime-heartbeat/0.1`
- `session-create-request/0.1`
- `session/0.1`
- `turn-start-request/0.1`
- `turn-started/0.1`
- `workspace-edit/0.1`
- `workspace-edit-preview/0.1`
- `timeline-event/0.1`
- `model-catalog/0.1`
- `model-catalog-cache/0.1`
- `model-profile/0.1`

## Security

### Transport Security

**stdio** (default):
- `local: true`
- `authenticated: false`
- `encrypted: false`
- `peer_verified: false`

**Unix socket** (macOS):
- `local: true`
- `authenticated: false`
- `encrypted: false`
- `peer_verified: true`

**Named pipe** (Windows):
- `local: true`
- `authenticated: false`
- `encrypted: false`
- `peer_verified: true`

### Permission Enforcement

1. Capability negotiation during initialize
2. Per-method capability check
3. Per-operation permission check
4. Approval workflow for mutations
5. Audit trail for all actions

## Best Practices

### Client Implementation

1. **Always negotiate capabilities** - Check negotiated capabilities before calling methods
2. **Handle reconnection** - Implement reconnection logic with state recovery
3. **Use heartbeat** - Monitor connection liveness
4. **Validate schemas** - Check schema_version in responses
5. **Handle errors gracefully** - All error codes have specific meanings
6. **Respect frame limits** - 4 MiB maximum per frame
7. **Subscribe to timeline** - Use subscription for real-time updates
8. **Batch requests** - Use request IDs to track multiple concurrent requests

### Error Handling

```typescript
try {
  const result = await client.request(method, params);
} catch (error) {
  switch (error.code) {
    case -32001:
      // Not initialized - reconnect
      await reconnect();
      break;
    case -32002:
      // Capability not negotiated - feature unavailable
      showFeatureUnavailable();
      break;
    case -32004:
      // Overload - retry with backoff
      await delay(1000);
      retry();
      break;
    default:
      // Other error
      handleError(error);
  }
}
```

### Performance

1. **Use subscription** - More efficient than polling
2. **Batch operations** - Combine multiple operations when possible
3. **Cache responses** - Cache catalog, profiles, etc.
4. **Debounce requests** - Avoid excessive requests
5. **Stream large content** - Use artifacts for large data

## Resources

- [AAP Protocol Guide](AAP-PROTOCOL-GUIDE.md) - Complete protocol specification
- [AAP Adapter Contributor Guide](AAP-ADAPTER-CONTRIBUTOR-GUIDE.md) - Adapter development
- [Architecture](../ARCHITECTURE.md) - System architecture
- [Security Documentation](SECURITY-DOCUMENTATION.md) - Security model
