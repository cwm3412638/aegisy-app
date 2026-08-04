//! One-time host/sidecar bootstrap authentication prelude (OpenSpec task `4.4`).
//!
//! When the Qt host launches the sidecar it generates one cryptographically
//! random 32-byte token per process generation and passes it through the
//! `AEGISY_BOOTSTRAP_TOKEN` environment variable. Environment variables are
//! not process arguments, and the token never enters ordinary logs: the
//! sidecar removes the variable from its own environment before constructing
//! any Runtime, adapter, terminal, or Git child, so no descendant process can
//! inherit it.
//!
//! A token-configured sidecar requires the first line on the connection to be
//! the exact prelude frame
//! `{"schema":"aegisy-bootstrap-auth/0.1","token":"<base64url-no-pad>"}`.
//! The prelude is a transport-level boundary, not an AAP message: it is
//! consumed before the ordinary frame loop, never reaches the JSON-RPC
//! dispatcher, and does not change the stable `0.1` wire schema. A missing,
//! malformed, oversized, replayed, or mismatched prelude fails closed with the
//! fixed content-free `-32154` error and terminates the connection before any
//! Runtime, Store, or adapter state is constructed from client input.
//!
//! A sidecar started without the environment token keeps the legacy
//! unauthenticated local behavior (`authenticated: false`) so developer
//! fixtures and standalone runs remain unchanged. Authentication never grants
//! encryption, peer verification, permission, approval, mutation, or execution
//! authority; the Agent/Codex read-only boundary is unchanged.

use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use base64::Engine;
use serde_json::{json, Value};

/// Environment variable carrying the one-time bootstrap token from the Qt host.
pub const BOOTSTRAP_TOKEN_ENV: &str = "AEGISY_BOOTSTRAP_TOKEN";
/// Exact schema label required in the prelude frame.
pub const BOOTSTRAP_PRELUDE_SCHEMA: &str = "aegisy-bootstrap-auth/0.1";
/// Stable content-free JSON-RPC error code for bootstrap authentication failure.
pub const BOOTSTRAP_AUTH_ERROR_CODE: i64 = -32154;
/// The complete prelude frame is 93 bytes; anything above this bound is invalid.
pub const BOOTSTRAP_PRELUDE_MAX_BYTES: usize = 128;
/// Raw bootstrap token length in bytes.
pub const BOOTSTRAP_TOKEN_BYTES: usize = 32;
/// Base64url no-pad encoding of a 32-byte token is exactly 43 characters.
pub const BOOTSTRAP_TOKEN_ENCODED_CHARS: usize = 43;

const PRELUDE_PREFIX: &str = "{\"schema\":\"aegisy-bootstrap-auth/0.1\",\"token\":\"";
const PRELUDE_SUFFIX: &str = "\"}";

/// Content-free bootstrap authentication failure classes. The token, the
/// provided value, and environment details are never reported.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BootstrapAuthError {
    /// The configured environment value was not a canonical token encoding.
    InvalidEnvironment,
    /// The first frame was missing, oversized, or not the exact prelude shape.
    MalformedPrelude,
    /// The decoded token did not match the configured token, or the token was
    /// already consumed.
    TokenMismatch,
}

impl BootstrapAuthError {
    pub fn code(self) -> &'static str {
        match self {
            Self::InvalidEnvironment => "bootstrap-auth-invalid-environment",
            Self::MalformedPrelude => "bootstrap-auth-malformed-prelude",
            Self::TokenMismatch => "bootstrap-auth-token-mismatch",
        }
    }
}

/// Fixed content-free JSON-RPC error written before the connection closes.
/// It contains no token bytes, no provided value, and no environment detail.
pub fn bootstrap_auth_error_response() -> Value {
    json!({
        "jsonrpc": "2.0",
        "id": null,
        "error": {
            "code": BOOTSTRAP_AUTH_ERROR_CODE,
            "message": "AAP bootstrap authentication failed"
        }
    })
}

fn decode_token(encoded: &str) -> Result<[u8; BOOTSTRAP_TOKEN_BYTES], BootstrapAuthError> {
    if encoded.len() != BOOTSTRAP_TOKEN_ENCODED_CHARS {
        return Err(BootstrapAuthError::InvalidEnvironment);
    }
    let decoded = URL_SAFE_NO_PAD
        .decode(encoded)
        .map_err(|_| BootstrapAuthError::InvalidEnvironment)?;
    if decoded.len() != BOOTSTRAP_TOKEN_BYTES {
        return Err(BootstrapAuthError::InvalidEnvironment);
    }
    // Reject non-canonical encodings whose unused trailing bits are set.
    if URL_SAFE_NO_PAD.encode(&decoded) != encoded {
        return Err(BootstrapAuthError::InvalidEnvironment);
    }
    let mut token = [0u8; BOOTSTRAP_TOKEN_BYTES];
    token.copy_from_slice(&decoded);
    Ok(token)
}

/// Strictly parse the exact prelude frame shape and return the decoded token.
/// A byte-level parser is used so duplicate keys, extra keys, whitespace,
/// nested JSON, or any other deviation is rejected by construction.
fn parse_prelude(frame: &[u8]) -> Result<[u8; BOOTSTRAP_TOKEN_BYTES], BootstrapAuthError> {
    if frame.len() > BOOTSTRAP_PRELUDE_MAX_BYTES {
        return Err(BootstrapAuthError::MalformedPrelude);
    }
    let frame = std::str::from_utf8(frame).map_err(|_| BootstrapAuthError::MalformedPrelude)?;
    let encoded = frame
        .strip_prefix(PRELUDE_PREFIX)
        .and_then(|rest| rest.strip_suffix(PRELUDE_SUFFIX))
        .ok_or(BootstrapAuthError::MalformedPrelude)?;
    if encoded.len() != BOOTSTRAP_TOKEN_ENCODED_CHARS
        || !encoded
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
    {
        return Err(BootstrapAuthError::MalformedPrelude);
    }
    decode_token(encoded).map_err(|_| BootstrapAuthError::MalformedPrelude)
}

fn constant_time_eq(
    left: &[u8; BOOTSTRAP_TOKEN_BYTES],
    right: &[u8; BOOTSTRAP_TOKEN_BYTES],
) -> bool {
    let mut difference = 0u8;
    for index in 0..BOOTSTRAP_TOKEN_BYTES {
        difference |= left[index] ^ right[index];
    }
    std::hint::black_box(difference) == 0
}

/// One-time bootstrap token owned by the sidecar process. The token is
/// consumed by the first successful verification and zeroed on consumption
/// and on drop; it grants no authority beyond the authenticated transport
/// fact reported in the AAP handshake.
pub struct BootstrapToken {
    token: [u8; BOOTSTRAP_TOKEN_BYTES],
    used: bool,
}

impl BootstrapToken {
    /// Read and immediately remove `AEGISY_BOOTSTRAP_TOKEN` from the process
    /// environment. `Ok(None)` selects the legacy unauthenticated local mode;
    /// a present but malformed value is a startup error.
    pub fn from_environment() -> Result<Option<Self>, BootstrapAuthError> {
        let Some(value) = std::env::var_os(BOOTSTRAP_TOKEN_ENV) else {
            return Ok(None);
        };
        std::env::remove_var(BOOTSTRAP_TOKEN_ENV);
        let encoded = value
            .to_str()
            .ok_or(BootstrapAuthError::InvalidEnvironment)?
            .to_owned();
        let token = decode_token(&encoded)?;
        Ok(Some(Self { token, used: false }))
    }

    /// Verify one candidate prelude frame. A successful verification consumes
    /// the token; any later verification fails closed.
    pub fn verify_prelude_frame(&mut self, frame: &[u8]) -> Result<(), BootstrapAuthError> {
        let provided = parse_prelude(frame)?;
        if self.used || !constant_time_eq(&self.token, &provided) {
            return Err(BootstrapAuthError::TokenMismatch);
        }
        self.used = true;
        self.zeroize();
        Ok(())
    }

    fn zeroize(&mut self) {
        for byte in &mut self.token {
            std::hint::black_box(&mut *byte);
            *byte = 0;
        }
    }
}

impl Drop for BootstrapToken {
    fn drop(&mut self) {
        self.zeroize();
    }
}

#[cfg(test)]
mod tests {
    use super::{
        bootstrap_auth_error_response, BootstrapAuthError, BootstrapToken,
        BOOTSTRAP_AUTH_ERROR_CODE, BOOTSTRAP_PRELUDE_MAX_BYTES, PRELUDE_PREFIX, PRELUDE_SUFFIX,
    };
    use base64::engine::general_purpose::URL_SAFE_NO_PAD;
    use base64::Engine;

    fn token_bytes() -> [u8; 32] {
        let mut token = [0u8; 32];
        for (index, byte) in token.iter_mut().enumerate() {
            *byte = u8::try_from(index).unwrap() ^ 0xA5;
        }
        token
    }

    fn token_string() -> String {
        URL_SAFE_NO_PAD.encode(token_bytes())
    }

    fn prelude(encoded: &str) -> Vec<u8> {
        format!("{PRELUDE_PREFIX}{encoded}{PRELUDE_SUFFIX}").into_bytes()
    }

    fn configured_token() -> BootstrapToken {
        BootstrapToken {
            token: token_bytes(),
            used: false,
        }
    }

    #[test]
    fn environment_lifecycle_reads_clears_and_rejects_malformed_values() {
        std::env::set_var(super::BOOTSTRAP_TOKEN_ENV, token_string());
        let token = BootstrapToken::from_environment().unwrap().unwrap();
        assert!(std::env::var_os(super::BOOTSTRAP_TOKEN_ENV).is_none());
        drop(token);
        assert!(BootstrapToken::from_environment().unwrap().is_none());

        for value in ["", "short", &"A".repeat(44), &"!".repeat(43)] {
            std::env::set_var(super::BOOTSTRAP_TOKEN_ENV, value);
            assert!(matches!(
                BootstrapToken::from_environment(),
                Err(BootstrapAuthError::InvalidEnvironment)
            ));
            assert!(std::env::var_os(super::BOOTSTRAP_TOKEN_ENV).is_none());
        }
        // Non-canonical trailing bits in the final character are rejected:
        // flipping only the lowest (padding) bit preserves the decoded bytes
        // but is not the canonical encoding.
        let mut non_canonical = token_string().into_bytes();
        let last = non_canonical.len() - 1;
        non_canonical[last] ^= 1;
        std::env::set_var(
            super::BOOTSTRAP_TOKEN_ENV,
            String::from_utf8(non_canonical).unwrap(),
        );
        assert!(matches!(
            BootstrapToken::from_environment(),
            Err(BootstrapAuthError::InvalidEnvironment)
        ));
    }

    #[test]
    fn exact_prelude_verifies_once_and_consumes_the_token() {
        let mut token = configured_token();
        token
            .verify_prelude_frame(&prelude(&token_string()))
            .unwrap();
        assert_eq!(
            token.verify_prelude_frame(&prelude(&token_string())),
            Err(BootstrapAuthError::TokenMismatch)
        );
    }

    #[test]
    fn wrong_token_fails_closed() {
        let mut wrong = token_bytes();
        wrong[0] ^= 0xFF;
        let mut token = configured_token();
        assert_eq!(
            token.verify_prelude_frame(&prelude(&URL_SAFE_NO_PAD.encode(wrong))),
            Err(BootstrapAuthError::TokenMismatch)
        );
        // The configured token remains usable after a mismatch.
        token
            .verify_prelude_frame(&prelude(&token_string()))
            .unwrap();
    }

    #[test]
    fn malformed_preclude_shapes_fail_closed() {
        let encoded = token_string();
        let mut token = configured_token();
        let cases: Vec<Vec<u8>> = vec![
            Vec::new(),
            b"{}".to_vec(),
            prelude(&encoded[..encoded.len() - 1]),
            prelude(&format!("{encoded}A")),
            format!(" {{\"schema\":\"aegisy-bootstrap-auth/0.1\",\"token\":\"{encoded}\"}}")
                .into_bytes(),
            format!("{{\"token\":\"{encoded}\",\"schema\":\"aegisy-bootstrap-auth/0.1\"}}")
                .into_bytes(),
            format!(
                "{{\"schema\":\"aegisy-bootstrap-auth/0.1\",\"token\":\"{encoded}\",\"extra\":1}}"
            )
            .into_bytes(),
            format!("{{\"schema\":\"aegisy-bootstrap-auth/0.2\",\"token\":\"{encoded}\"}}")
                .into_bytes(),
            vec![b'x'; BOOTSTRAP_PRELUDE_MAX_BYTES + 1],
            prelude(&format!("+{}", &encoded[1..])),
            prelude(&format!("/{}", &encoded[1..])),
            vec![0xFF, 0xFE, 0xFD],
        ];
        for case in cases {
            assert!(matches!(
                token.verify_prelude_frame(&case),
                Err(BootstrapAuthError::MalformedPrelude) | Err(BootstrapAuthError::TokenMismatch)
            ));
        }
        token.verify_prelude_frame(&prelude(&encoded)).unwrap();
    }

    #[test]
    fn failure_response_is_fixed_content_free_and_bounded() {
        let response = bootstrap_auth_error_response();
        assert_eq!(response["jsonrpc"], "2.0");
        assert!(response["id"].is_null());
        assert_eq!(response["error"]["code"], BOOTSTRAP_AUTH_ERROR_CODE);
        let encoded = serde_json::to_vec(&response).unwrap();
        assert!(encoded.len() < 160);
        let text = String::from_utf8(encoded).unwrap();
        assert!(!text.contains(&token_string()));
        assert!(!text.contains("AEGISY_BOOTSTRAP_TOKEN"));
    }

    #[test]
    fn consumed_token_is_zeroed() {
        let mut token = configured_token();
        token
            .verify_prelude_frame(&prelude(&token_string()))
            .unwrap();
        assert!(token.token.iter().all(|byte| *byte == 0));
    }
}
