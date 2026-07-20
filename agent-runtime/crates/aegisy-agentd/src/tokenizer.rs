//! Conservative token estimates for context budgeting.
//!
//! Aegisy does not claim provider-specific tokenizer authority until a reviewed
//! adapter is available. The fallback is deliberately byte-based, content-free,
//! and marked as an estimate so callers cannot confuse it with a model window.

use serde::Serialize;

pub const SCHEMA_VERSION: &str = "tokenizer/0.1";
pub const UNKNOWN_TOKENIZER_ID: &str = "unknown-utf8-four-byte";

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct TokenizerSummary {
    pub schema_version: &'static str,
    pub tokenizer_id: &'static str,
    pub authority: &'static str,
    pub exact: bool,
    pub provider_window_authoritative: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TokenEstimate {
    pub bytes: usize,
    pub tokens: u64,
}

pub fn summary() -> TokenizerSummary {
    TokenizerSummary {
        schema_version: SCHEMA_VERSION,
        tokenizer_id: UNKNOWN_TOKENIZER_ID,
        authority: "conservative-unknown",
        exact: false,
        provider_window_authoritative: false,
    }
}

pub fn estimate_bytes(bytes: usize) -> TokenEstimate {
    let value = u64::try_from(bytes).unwrap_or(u64::MAX);
    let tokens = (value / 4).saturating_add(u64::from(value % 4 != 0));
    TokenEstimate { bytes, tokens }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unknown_tokenizer_is_explicitly_conservative() {
        let metadata = summary();
        assert_eq!(metadata.schema_version, "tokenizer/0.1");
        assert_eq!(metadata.tokenizer_id, "unknown-utf8-four-byte");
        assert_eq!(metadata.authority, "conservative-unknown");
        assert!(!metadata.exact);
        assert!(!metadata.provider_window_authoritative);
    }

    #[test]
    fn byte_estimate_rounds_up_without_overflow() {
        assert_eq!(estimate_bytes(0).tokens, 0);
        assert_eq!(estimate_bytes(1).tokens, 1);
        assert_eq!(estimate_bytes(4).tokens, 1);
        assert_eq!(estimate_bytes(5).tokens, 2);
        assert_eq!(estimate_bytes(usize::MAX).tokens, u64::MAX / 4 + 1);
    }
}
