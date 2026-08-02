# Task 4.4: Bootstrap Authentication Implementation Plan

## Overview

Implement one-time host/sidecar bootstrap authentication without secrets in process arguments or ordinary logs.

## Current State

- ✅ PID/UID peer verification working
- ✅ Transport security (Unix socket/named pipe)
- ❌ No cryptographic authentication
- ❌ `authenticated: false` in handshake

## Requirements

### Security Requirements
1. Generate cryptographically secure one-time token
2. Pass token without exposing in process arguments
3. Pass token without exposing in ordinary logs
4. Verify token on first AAP frame
5. Invalidate token after successful use
6. Reject missing/wrong/replayed tokens
7. Report `authenticated: true` after verification

### Token Specifications
- **Length**: 32 bytes (256 bits)
- **Encoding**: Base64 or hex
- **Lifetime**: Single use, process generation scoped
- **Passing**: Environment variable (cleared after read)

## Implementation Design

### 1. Token Generation (Qt Host)

**File**: `workbench/src/agent_runtime_supervisor.cpp`

```cpp
// Generate 32-byte random token
QByteArray generateBootstrapToken() {
    QByteArray token(32, Qt::Uninitialized);
    if (!QRandomGenerator::system()->fillRange(
        reinterpret_cast<quint32*>(token.data()), 
        token.size() / sizeof(quint32))) {
        return QByteArray(); // Generation failed
    }
    return token.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}
```

### 2. Token Passing (Qt Host)

**File**: `workbench/src/agent_runtime_supervisor.cpp`

```cpp
// Set environment variable before process start
QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
env.insert("AEGISY_BOOTSTRAP_TOKEN", bootstrapToken);
process->setProcessEnvironment(env);

// Store token hash for verification
QString tokenHash = QCryptographicHash::hash(
    QByteArray::fromBase64(bootstrapToken.toUtf8()),
    QCryptographicHash::Sha256
).toHex();
```

### 3. Token Reception (Rust Sidecar)

**File**: `agent-runtime/crates/aegisy-agentd/src/bootstrap_auth.rs`

```rust
use std::env;

pub struct BootstrapToken {
    token: [u8; 32],
    used: bool,
}

impl BootstrapToken {
    pub fn from_environment() -> Result<Self, BootstrapError> {
        let token_b64 = env::var("AEGISY_BOOTSTRAP_TOKEN")
            .map_err(|_| BootstrapError::Missing)?;
        
        // Clear from environment immediately
        env::remove_var("AEGISY_BOOTSTRAP_TOKEN");
        
        // Decode and validate
        let token_bytes = base64::decode_config(
            token_b64, 
            base64::URL_SAFE_NO_PAD
        ).map_err(|_| BootstrapError::Invalid)?;
        
        if token_bytes.len() != 32 {
            return Err(BootstrapError::InvalidLength);
        }
        
        let mut token = [0u8; 32];
        token.copy_from_slice(&token_bytes);
        
        Ok(Self { token, used: false })
    }
    
    pub fn verify(&mut self, provided: &[u8]) -> bool {
        if self.used {
            return false; // Already used
        }
        
        let valid = constant_time_eq(&self.token, provided);
        if valid {
            self.used = true;
        }
        valid
    }
}
```

### 4. Token Verification (First AAP Frame)

**File**: `agent-runtime/crates/aegisy-agentd/src/transport.rs`

```rust
pub struct AuthenticatedTransport {
    bootstrap: Option<BootstrapToken>,
    authenticated: bool,
}

impl AuthenticatedTransport {
    pub fn verify_first_frame(&mut self, frame: &[u8]) -> Result<(), AuthError> {
        if self.authenticated {
            return Ok(()); // Already authenticated
        }
        
        // Extract auth header from first frame
        let auth_token = extract_auth_header(frame)?;
        
        match &mut self.bootstrap {
            Some(token) => {
                if token.verify(auth_token) {
                    self.authenticated = true;
                    self.bootstrap = None; // Clear token
                    Ok(())
                } else {
                    Err(AuthError::InvalidToken)
                }
            }
            None => Err(AuthError::NoBootstrapToken)
        }
    }
}
```

### 5. Handshake Update

**File**: `agent-runtime/crates/aegisy-agentd/src/handshake.rs`

```rust
// Update initialize response
let transport_security = json!({
    "transport": transport_type,
    "local": true,
    "peer_verified": peer_verified,
    "authenticated": self.transport.is_authenticated(), // Now true
    "encrypted": false
});
```

### 6. Qt Client Update

**File**: `workbench/src/aap_client.cpp`

```cpp
// Add auth header to first frame (initialize request)
QJsonObject addAuthHeader(const QJsonObject& frame, const QString& token) {
    QJsonObject authed = frame;
    authed["_auth"] = token; // Temporary auth field
    return authed;
}

// Send initialize with auth
void AapClient::initialize() {
    QJsonObject initRequest = buildInitializeRequest();
    QJsonObject authedRequest = addAuthHeader(initRequest, bootstrapToken_);
    sendFrame(authedRequest);
    bootstrapToken_.clear(); // Clear after use
}
```

## Testing Requirements

### Unit Tests

1. **Token Generation**
   - Test token length (32 bytes)
   - Test uniqueness (generate 1000, all different)
   - Test encoding (valid base64)

2. **Token Verification**
   - Test correct token → authenticated
   - Test wrong token → rejected
   - Test missing token → rejected
   - Test replayed token → rejected
   - Test token after use → rejected

3. **Environment Handling**
   - Test token cleared after read
   - Test missing env var → error
   - Test malformed token → error

### Integration Tests

1. **E2E Authentication**
   - Qt generates token
   - Sidecar receives and clears
   - First frame authenticated
   - Handshake reports `authenticated: true`

2. **Failure Cases**
   - Missing token → connection refused
   - Wrong token → connection refused
   - Replayed token → connection refused
   - Token in logs → test fails

## Files to Modify

### Rust (Sidecar)
- `agent-runtime/crates/aegisy-agentd/src/bootstrap_auth.rs` (new)
- `agent-runtime/crates/aegisy-agentd/src/transport.rs`
- `agent-runtime/crates/aegisy-agentd/src/handshake.rs`
- `agent-runtime/crates/aegisy-agentd/src/macos_unix_socket.rs`
- `agent-runtime/crates/aegisy-agentd/src/windows_named_pipe.rs`

### C++ (Qt Host)
- `workbench/src/agent_runtime_supervisor.h`
- `workbench/src/agent_runtime_supervisor.cpp`
- `workbench/src/aap_client.h`
- `workbench/src/aap_client.cpp`

### Tests
- `agent-runtime/crates/aegisy-agentd/tests/bootstrap_auth_test.rs` (new)
- `workbench/tests/test_authenticated_transport.cpp` (new)

## Implementation Steps

1. ✅ Create implementation plan (this document)
2. ⬜ Implement Rust bootstrap_auth module
3. ⬜ Update Rust transport to verify first frame
4. ⬜ Update Rust handshake to report authenticated
5. ⬜ Implement Qt token generation
6. ⬜ Update Qt to pass token via environment
7. ⬜ Update Qt to send auth header in first frame
8. ⬜ Add Rust unit tests
9. ⬜ Add Qt unit tests
10. ⬜ Add E2E integration test
11. ⬜ Verify no tokens in logs
12. ⬜ Update task 4.4 to complete

## Security Considerations

### ✅ Secure
- Token generated with cryptographic RNG
- Token passed via environment (not process args)
- Token cleared immediately after read
- Token verified with constant-time comparison
- Token single-use only
- Token scoped to process generation

### ❌ Not Secure (Out of Scope)
- No encryption (transport is local)
- No mutual authentication (host trusts sidecar by PID)
- No token rotation (single process lifetime)
- No revocation (process-scoped)

## Completion Criteria

- [ ] Rust bootstrap_auth module implemented
- [ ] Qt token generation implemented
- [ ] Token passed via environment variable
- [ ] Token verified on first frame
- [ ] Handshake reports `authenticated: true`
- [ ] Unit tests pass (Rust + Qt)
- [ ] Integration test passes
- [ ] No tokens in logs (verified)
- [ ] Task 4.4 marked complete

---

**Status**: Plan complete, ready for implementation
**Estimated Effort**: 4-6 hours
**Dependencies**: None (4.1, 4.2 already complete)
**Blocks**: 4.8 (hostile client tests need auth to test)
