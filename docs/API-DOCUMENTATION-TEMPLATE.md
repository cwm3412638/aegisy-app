# AAP Protocol Method Documentation Template

## Method Name

### Signature
```
return_type method_name(param_type param_name, ...)
```

### Parameters
| Name | Type | Constraints | Description |
|------|------|-------------|-------------|
| param_name | type | constraints | description |

### Return Values
| Type | Description |
|------|-------------|
| type | description |

### Error Codes
| Code | Condition | Description |
|------|-----------|-------------|
| code | condition | description |

### Examples

**Request:**
```json
{
  "method": "method_name",
  "params": {}
}
```

**Response:**
```json
{
  "result": {}
}
```

### Capabilities Required
- capability_name

### Security Considerations
- consideration

### Implementation Notes
- note

---

## Example: verify_artifact_signature

### Signature
```cpp
VerificationResult verify_artifact_signature(
    const std::vector<uint8_t>& artifact_data,
    const std::vector<uint8_t>& signature,
    const SigningKeyRing& key_ring
)
```

### Parameters
| Name | Type | Constraints | Description |
|------|------|-------------|-------------|
| artifact_data | std::vector<uint8_t> | Non-empty | Raw artifact bytes to verify |
| signature | std::vector<uint8_t> | Valid signature format | Cryptographic signature |
| key_ring | SigningKeyRing | Contains valid keys | Key ring for verification |

### Return Values
| Type | Description |
|------|-------------|
| VerificationResult | Contains verification status, matched key ID, and error details |

### Error Codes
| Code | Condition | Description |
|------|-----------|-------------|
| INVALID_SIGNATURE | Signature verification fails | Signature does not match artifact data |
| NO_MATCHING_KEY | No key in ring verifies signature | Key ring lacks the signing key |
| MALFORMED_DATA | Input data invalid | Artifact data or signature corrupted |

### Examples

**Request:**
```cpp
std::vector<uint8_t> artifact = load_artifact("update.bin");
std::vector<uint8_t> sig = load_signature("update.sig");
SigningKeyRing ring = load_key_ring();

auto result = verify_artifact_signature(artifact, sig, ring);
```

**Response:**
```cpp
// Success case
VerificationResult {
    .verified = true,
    .key_id = "key-2024-prod",
    .error = std::nullopt
}

// Failure case
VerificationResult {
    .verified = false,
    .key_id = std::nullopt,
    .error = "NO_MATCHING_KEY"
}
```

### Capabilities Required
- artifact.verify
- keyring.read

### Security Considerations
- Verify artifact data integrity before signature check
- Use constant-time comparison for signature verification
- Key ring must be loaded from trusted source
- Cache verification results only with artifact hash binding

### Implementation Notes
- Supports Ed25519 and RSA-4096 signatures
- Key ring lookup is O(n) over active keys
- Signature format follows RFC 8032 for Ed25519
- Failed verifications are logged for audit
