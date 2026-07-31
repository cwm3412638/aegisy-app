# Security Audit Checklist

## 1. Code Security

### Input Validation
- [ ] **AAP Transport JSON parsing** (`agent-runtime/aap-schema/runtime/cpp/aap_transport_runtime.h`)
  - Verify size limits enforced: `kMaxTransportJsonBytes` (4MB), `kMaxTransportJsonDepth` (128), `kMaxTransportJsonNodes` (65,536)
  - Check integer range validation: `kMaxTransportJsonSafeInteger` / `kMinTransportJsonSafeInteger`
  - Validate `parseTransportJsonRaw()` and `parseTransportJsonRawDetailed()` error handling
  - Confirm `TransportSchemaRuntime::validateDefinitionRaw()` rejects malformed input

- [ ] **Path validation** (`include/canonical_path_policy.h`)
  - Test `normalized()` handles edge cases (empty paths, drive roots, trailing slashes)
  - Verify `isStrictDescendant()` prevents directory traversal attacks
  - Check case sensitivity handling for Windows vs POSIX systems

- [ ] **API input sanitization** (`include/api_client.h`, `src/api_client.cpp`)
  - Review all `post()`, `put()`, `get()` methods for input validation
  - Check JSON parsing in `parseResponse()` handles malformed data
  - Verify authentication token format validation

### Injection Prevention
- [ ] **Command injection** (`include/process_command.h`, `src/process_command.cpp`)
  - Audit all `QProcess` usage for shell injection vulnerabilities
  - Verify arguments are properly escaped/quoted
  - Check environment variable sanitization

- [ ] **SQL injection** (Qt SQL usage)
  - Confirm prepared statements used throughout
  - Check `rusqlite` usage in Rust runtime (`agent-runtime/Cargo.toml`)

- [ ] **JSON injection**
  - Verify all JSON construction uses proper API methods, not string concatenation
  - Check `QJsonObject`, `QJsonArray` usage patterns

### Buffer Overflows
- [ ] **Qt container bounds checking**
  - Review `QByteArray`, `QString`, `QVector` usage for unchecked access
  - Check array indexing in AAP transport code

- [ ] **Rust memory safety** (`agent-runtime/`)
  - Verify no `unsafe` blocks without justification
  - Check FFI boundaries for proper validation

## 2. Credential Management

### Storage
- [ ] **Platform credential stores** (`include/secure_storage.h`, `src/secure_storage.cpp`)
  - macOS: Verify Keychain API usage (`saveToKeychain`, `loadFromKeychain`, `deleteFromKeychain`)
  - Windows: Audit DPAPI implementation (`encryptWindows`, `decryptWindows`)
  - Linux: Check Secret Service integration (`saveToSecretService`, `loadFromSecretService`)
  - Verify `isAvailable()` correctly detects platform support
  - Test `contains()` doesn't leak credential existence timing

- [ ] **Token handling**
  - Verify `saveToken()` / `loadToken()` use platform-specific encryption
  - Check tokens never logged or written to disk unencrypted
  - Audit `clearToken()` securely wipes memory

### Transmission
- [ ] **API authentication** (`include/api_client.h`)
  - Verify `setAuthToken()` stores tokens securely in memory
  - Check all API requests use HTTPS (verify `m_baseUrl` validation)
  - Confirm Bearer tokens sent in Authorization header, not URL parameters
  - Review `loginSuccess` signal doesn't expose credentials

- [ ] **Gateway authentication** (`include/gateway_manager.h`)
  - Audit `localToken` generation for sufficient entropy
  - Verify token transmitted securely to gateway process
  - Check `configureProfile()` handles API keys securely

### Rotation
- [ ] **Token lifecycle**
  - Verify `authenticationExpired` signal triggers re-authentication
  - Check expired tokens are cleared from storage
  - Test token refresh mechanisms

## 3. IPC Security

### AAP Transport
- [ ] **Transport modes** (`include/agent_runtime_client.h`)
  - Verify `TransportMode::VerifiedUnixSocket` validates socket ownership/permissions
  - Check `TransportMode::VerifiedWindowsNamedPipe` validates pipe security descriptor
  - Audit `TransportMode::Stdio` input/output sanitization

- [ ] **Message validation**
  - Verify all incoming messages validated against schema
  - Check message size limits enforced
  - Test malformed message handling doesn't crash

### Authentication
- [ ] **Runtime client authentication** (`include/agent_runtime_client.h`)
  - Verify `sanitizedSidecarEnvironment()` removes credential-bearing variables
  - Check heartbeat mechanism prevents unauthorized connections
  - Audit `isReady()` / `isControlAvailable()` authorization checks

### Encryption
- [ ] **Transport encryption**
  - Verify Unix socket permissions restrict access (0600)
  - Check Windows named pipe ACLs properly configured
  - Audit stdio transport for sensitive data exposure

## 4. Filesystem Security

### Path Validation
- [ ] **Canonical path enforcement** (`include/canonical_path_policy.h`)
  - Test `normalized()` prevents `..` traversal
  - Verify `isStrictDescendant()` used before file operations
  - Check Windows UNC path handling
  - Test symlink resolution doesn't escape boundaries

- [ ] **Artifact manifest verification** (`include/artifact_manifest.h`)
  - Audit `verifyFile()` validates paths before access
  - Check `verifyObject()` enforces `baseDirectory` containment
  - Verify SHA-256 hashes validated for all artifacts
  - Test file size limits enforced (`runtimeSizeBytes`, `adapterSizeBytes`)

### Permissions
- [ ] **File creation**
  - Verify files created with restrictive permissions (0600 for sensitive data)
  - Check directory permissions (0700 for user-only directories)
  - Audit temporary file creation for race conditions

- [ ] **File access**
  - Verify read operations check permissions before access
  - Check write operations validate target writability
  - Test privilege escalation attempts fail

### Symlinks
- [ ] **Symlink handling**
  - Verify symlinks resolved before path validation
  - Check operations don't follow symlinks to unauthorized locations
  - Test TOCTOU (time-of-check-time-of-use) vulnerabilities

## 5. Network Security

### TLS
- [ ] **TLS configuration** (`src/api_client.cpp`, `tests/windows_tls_probe.cpp`)
  - Verify minimum TLS version (TLS 1.2+)
  - Check cipher suite configuration excludes weak ciphers
  - Audit `QNetworkAccessManager` SSL configuration

### Certificate Validation
- [ ] **Certificate verification**
  - Verify certificate chain validation enabled
  - Check hostname verification enforced
  - Test self-signed certificate rejection
  - Audit certificate pinning if implemented

### Host Verification
- [ ] **API endpoint validation**
  - Verify `setBaseUrl()` validates URL format
  - Check domain whitelist if applicable
  - Test redirect handling doesn't bypass validation

## 6. Update Security

### Signature Verification
- [ ] **Update signing key ring** (`include/update_signing_key_ring.h`, `src/update_signing_key_ring.cpp`)
  - Verify `embeddedTrustAnchor()` uses hardcoded public key from CMake (`AEGISY_UPDATE_PUBLIC_KEY`)
  - Audit `verifyBootstrap()` validates initial key ring signature
  - Check `verifyRotation()` enforces generation monotonicity
  - Test `verifyEnvelopeChain()` validates complete chain
  - Verify `verifyArtifactSetSignature()` checks Ed25519 signatures
  - Check timestamp validation (`signedAtMs`, `nowMs`) prevents replay attacks
  - Test `requireCurrentlyActive` flag enforcement

- [ ] **Key ring cache** (`include/update_signing_key_ring_cache.h`, `src/update_signing_key_ring_cache.cpp`)
  - Verify cached authorities validated on load
  - Check cache invalidation on key rotation
  - Test cache poisoning resistance

### Rollback Protection
- [ ] **Version monotonicity**
  - Verify `generation` field enforces forward-only updates
  - Check downgrade attempts rejected
  - Test `idempotent` flag handling for re-application

- [ ] **Update progress tracking** (`include/update_progress_record.h`)
  - Verify partial updates can't be exploited
  - Check interrupted updates handled securely

### Artifact Verification
- [ ] **Manifest validation** (`include/artifact_manifest.h`)
  - Verify `manifestSha256` matches computed hash
  - Check `runtimeSha256` and `adapterSha256` validated
  - Test `runtimeFileIdentity` / `adapterFileIdentity` binding
  - Verify size limits prevent resource exhaustion

## 7. Dependency Security

### Vulnerability Scanning
- [ ] **C++ dependencies** (`CMakeLists.txt`)
  - Qt 5/6: Check for known CVEs in used version
  - OpenSSL: Verify version has no critical vulnerabilities
  - Review `find_package()` version constraints

- [ ] **Rust dependencies** (`agent-runtime/Cargo.toml`)
  - Run `cargo audit` for known vulnerabilities
  - Check pinned versions: `ed25519-dalek=2.1.1`, `rusqlite=0.40.1`, `sha2=0.10.9`
  - Verify `windows-sys=0.61.2` has no security issues

- [ ] **JavaScript dependencies** (`workbench-web/package.json`)
  - Run `npm audit` for vulnerabilities
  - Check `dompurify`, `monaco-editor`, `@xterm/xterm` versions
  - Verify `marked` (Markdown parser) is up-to-date

### Supply Chain
- [ ] **Dependency integrity**
  - Verify `package-lock.json` and `Cargo.lock` committed
  - Check dependency sources (crates.io, npm registry)
  - Audit build scripts in dependencies

- [ ] **Build reproducibility**
  - Verify release builds use pinned dependencies
  - Check CMake cache variables for security settings
  - Test builds produce consistent artifacts

## 8. Runtime Security

### Sandbox
- [ ] **Process isolation** (`include/agent_runtime_client.h`)
  - Verify runtime process runs with minimal privileges
  - Check `sanitizedSidecarEnvironment()` removes sensitive variables
  - Test filesystem access restricted to necessary paths

- [ ] **Gateway isolation** (`include/gateway_manager.h`)
  - Verify gateway process isolated from main application
  - Check `localToken` provides authentication boundary
  - Test gateway can't access application credentials

### Process Isolation
- [ ] **Child process management** (`include/process_command.h`)
  - Verify child processes inherit minimal environment
  - Check process termination handled securely
  - Test zombie process prevention

- [ ] **IPC boundaries**
  - Verify data crossing process boundaries validated
  - Check serialization/deserialization security
  - Test privilege boundaries enforced

### Privilege Separation
- [ ] **Least privilege principle**
  - Verify application runs as non-root/non-admin
  - Check elevated operations properly gated
  - Test privilege escalation attempts fail

- [ ] **Capability-based security**
  - Verify operations require explicit capabilities
  - Check capability delegation is minimal
  - Test unauthorized capability use rejected

## Testing Procedures

### Security Test Execution
- [ ] Run all tests in `tests/` directory
- [ ] Execute `tests/update_signing_key_ring_test.cpp` for signature verification
- [ ] Run `tests/artifact_manifest_test.cpp` for artifact validation
- [ ] Execute `tests/aap_transport_generated_types_test.cpp` for transport security
- [ ] Run `tests/agent_runtime_environment_test.cpp` for environment sanitization
- [ ] Execute `tests/tool_manager_gateway_config_test.cpp` for gateway security

### Manual Security Testing
- [ ] Attempt directory traversal attacks on file operations
- [ ] Test malformed JSON/AAP messages
- [ ] Attempt credential extraction from memory dumps
- [ ] Test TLS downgrade attacks
- [ ] Attempt update signature bypass
- [ ] Test symlink attacks on artifact installation

### Code Review Focus Areas
- [ ] All `QProcess::start()` calls
- [ ] All file I/O operations
- [ ] All network requests
- [ ] All credential storage/retrieval
- [ ] All signature verification paths
- [ ] All input parsing (JSON, paths, commands)

## Compliance Verification

- [ ] OWASP Top 10 coverage
- [ ] CWE Top 25 mitigation
- [ ] SANS Top 25 software errors addressed
- [ ] Platform-specific security guidelines (macOS, Windows, Linux)
