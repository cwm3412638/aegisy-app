//! Content-free classification for upstream HTTP/provider failures.
//!
//! The Codex App Server exposes a small, schema-driven `codexErrorInfo` object
//! when a provider or HTTP status is known.  Keep that authority as bounded
//! metadata instead of forwarding provider response bodies or error text into
//! AAP timeline data.

use serde::Serialize;
use serde_json::Value;

const SCHEMA_VERSION: &str = "provider-error/0.1";

/// A stable, body-free description of an upstream provider failure.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub(crate) struct ProviderError {
    pub schema_version: &'static str,
    pub source: &'static str,
    pub kind: &'static str,
    pub class: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub http_status: Option<u16>,
    pub retryable: bool,
    pub response_body_included: bool,
    pub credentials_included: bool,
}

impl ProviderError {
    fn new(
        kind: &'static str,
        class: &'static str,
        http_status: Option<u16>,
        retryable: bool,
    ) -> Self {
        Self {
            schema_version: SCHEMA_VERSION,
            source: "codex-app-server",
            kind,
            class,
            http_status,
            retryable,
            response_body_included: false,
            credentials_included: false,
        }
    }
}

/// Convert the pinned Codex `codexErrorInfo` union into the AAP error contract.
/// Unknown variants remain visible as a non-retryable provider error without
/// trusting arbitrary provider data.
pub(crate) fn from_codex_error_info(value: &Value) -> Option<ProviderError> {
    if let Some(status) = value.as_u64().and_then(|status| u16::try_from(status).ok()) {
        return Some(from_http_status(status));
    }
    let object = value.as_object();
    let status = object
        .filter(|object| object.len() == 1)
        .and_then(|object| object.values().next())
        .and_then(|variant| variant.get("httpStatusCode"))
        .and_then(Value::as_u64)
        .and_then(|status| u16::try_from(status).ok());

    if let Some(name) = value.as_str() {
        return Some(classify_named(name, status));
    }
    let object = object?;
    if object.len() != 1 {
        return Some(ProviderError::new(
            "provider-error",
            "provider",
            None,
            false,
        ));
    }
    let name = object.keys().next()?.as_str();
    Some(classify_named(name, status))
}

/// Classify an HTTP status when an upstream response exists but no provider
/// variant is available.  This is intentionally conservative: only statuses
/// that are commonly transient are retryable.
pub(crate) fn from_http_status(status: u16) -> ProviderError {
    match status {
        408 => ProviderError::new("request-timeout", "timeout", Some(status), true),
        401 | 403 => ProviderError::new("unauthorized", "provider", Some(status), false),
        400 | 404 | 405 | 406 | 409 | 413 | 415 | 422 => {
            ProviderError::new("bad-request", "provider", Some(status), false)
        }
        429 => ProviderError::new("rate-limit", "provider", Some(status), true),
        500 | 502 | 503 | 504 => {
            ProviderError::new("server-overloaded", "provider", Some(status), true)
        }
        _ if status >= 400 => ProviderError::new("http-error", "provider", Some(status), false),
        _ => ProviderError::new("http-error", "provider", Some(status), false),
    }
}

fn classify_named(name: &str, status: Option<u16>) -> ProviderError {
    match name {
        "contextWindowExceeded" => {
            ProviderError::new("context-window-exceeded", "budget", status, false)
        }
        "sessionBudgetExceeded" => {
            ProviderError::new("session-budget-exceeded", "budget", status, false)
        }
        "usageLimitExceeded" => {
            ProviderError::new("usage-limit-exceeded", "provider", status, false)
        }
        "serverOverloaded" => ProviderError::new("server-overloaded", "provider", status, true),
        "cyberPolicy" => ProviderError::new("cyber-policy", "policy", status, false),
        "internalServerError" => {
            ProviderError::new("internal-server-error", "provider", status, true)
        }
        "unauthorized" => ProviderError::new("unauthorized", "provider", status, false),
        "badRequest" => ProviderError::new("bad-request", "provider", status, false),
        "threadRollbackFailed" => {
            ProviderError::new("thread-rollback-failed", "provider", status, false)
        }
        "sandboxError" => ProviderError::new("sandbox-error", "sandbox", status, false),
        "httpConnectionFailed" => {
            ProviderError::new("http-connection-failed", "transport", status, true)
        }
        "responseStreamConnectionFailed" => ProviderError::new(
            "response-stream-connection-failed",
            "transport",
            status,
            true,
        ),
        "responseStreamDisconnected" => {
            ProviderError::new("response-stream-disconnected", "transport", status, true)
        }
        "responseTooManyFailedAttempts" => ProviderError::new(
            "response-too-many-failed-attempts",
            "transport",
            status,
            false,
        ),
        "activeTurnNotSteerable" => {
            ProviderError::new("active-turn-not-steerable", "policy", status, false)
        }
        _ => ProviderError::new("provider-error", "provider", status, false),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn codex_stream_disconnect_preserves_http_status_without_body() {
        let error = from_codex_error_info(&json!({
            "responseStreamDisconnected": { "httpStatusCode": 502 }
        }))
        .unwrap();
        assert_eq!(error.kind, "response-stream-disconnected");
        assert_eq!(error.class, "transport");
        assert_eq!(error.http_status, Some(502));
        assert!(error.retryable);
        assert!(!error.response_body_included);
        assert!(!error.credentials_included);
    }

    #[test]
    fn provider_variants_are_content_free_and_conservative() {
        let cases = [
            ("unauthorized", "provider", false),
            ("badRequest", "provider", false),
            ("serverOverloaded", "provider", true),
            ("contextWindowExceeded", "budget", false),
            ("sandboxError", "sandbox", false),
            ("other", "provider", false),
        ];
        for (name, class, retryable) in cases {
            let error = from_codex_error_info(&json!(name)).unwrap();
            assert_eq!(error.class, class, "{name}");
            assert_eq!(error.retryable, retryable, "{name}");
            let serialized = serde_json::to_string(&error).unwrap();
            assert!(!serialized.contains("provider response"));
            assert!(!serialized.contains("authorization"));
        }
    }

    #[test]
    fn http_status_policy_distinguishes_transient_and_terminal_failures() {
        assert!(from_http_status(429).retryable);
        assert!(from_http_status(503).retryable);
        assert!(from_http_status(408).retryable);
        assert!(!from_http_status(401).retryable);
        assert!(!from_http_status(422).retryable);
        assert_eq!(from_http_status(418).kind, "http-error");
    }

    #[test]
    fn malformed_codex_info_does_not_panic_or_accept_nested_body() {
        let error = from_codex_error_info(&json!({
            "unexpected": { "message": "provider response body" }
        }))
        .unwrap();
        assert_eq!(error.kind, "provider-error");
        assert!(!serde_json::to_string(&error)
            .unwrap()
            .contains("provider response body"));
        let ambiguous = from_codex_error_info(&json!({
            "responseStreamDisconnected": { "httpStatusCode": 502 },
            "unauthorized": {}
        }))
        .unwrap();
        assert_eq!(ambiguous.kind, "provider-error");
        assert_eq!(ambiguous.http_status, None);
        assert!(!ambiguous.retryable);
        assert!(from_codex_error_info(&Value::Null).is_none());
    }
}
