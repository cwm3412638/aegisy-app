#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct OutputRedactor {
    token: String,
    expectation: Expectation,
    discard_until_whitespace: bool,
    redacted_count: u64,
    source_bytes: u64,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
enum Expectation {
    #[default]
    None,
    SeparatorOrValue,
    Value,
}

impl OutputRedactor {
    pub fn push(&mut self, input: &str) -> String {
        self.source_bytes = self.source_bytes.saturating_add(input.len() as u64);
        let mut output = String::new();
        for character in input.chars() {
            if character.is_whitespace() {
                self.flush_token(&mut output);
                self.discard_until_whitespace = false;
                output.push(character);
                continue;
            }
            if self.discard_until_whitespace {
                continue;
            }
            self.token.push(character);
            if self.token.len() > 8 * 1024 {
                let rendered = self.render_token();
                if rendered == "[REDACTED]" || rendered.ends_with("[REDACTED]") {
                    output.push_str(&rendered);
                    self.discard_until_whitespace = true;
                } else {
                    output.push_str(&rendered);
                }
                self.token.clear();
            }
        }
        output
    }

    pub fn finish(&mut self) -> String {
        let mut output = String::new();
        self.flush_token(&mut output);
        output
    }

    pub fn redacted_count(&self) -> u64 {
        self.redacted_count
    }

    pub fn source_bytes(&self) -> u64 {
        self.source_bytes
    }

    fn flush_token(&mut self, output: &mut String) {
        if self.discard_until_whitespace {
            self.token.clear();
            return;
        }
        if !self.token.is_empty() {
            let rendered = self.render_token();
            output.push_str(&rendered);
            self.token.clear();
        }
    }

    fn render_token(&mut self) -> String {
        let token = self.token.clone();
        let normalized = token
            .trim_matches(|character: char| {
                matches!(
                    character,
                    '"' | '\'' | '`' | ',' | ';' | '(' | ')' | '[' | ']'
                )
            })
            .to_lowercase();

        match self.expectation {
            Expectation::Value => {
                if matches!(normalized.as_str(), "bearer" | "basic") {
                    return token;
                }
                self.expectation = Expectation::None;
                return self.redacted();
            }
            Expectation::SeparatorOrValue => {
                if matches!(normalized.as_str(), "=" | ":") {
                    self.expectation = Expectation::Value;
                    return token;
                }
                if normalized.starts_with('=') || normalized.starts_with(':') {
                    self.expectation = Expectation::None;
                    self.redacted_count = self.redacted_count.saturating_add(1);
                    return format!("{}[REDACTED]", &token[..1]);
                }
                self.expectation = Expectation::None;
                return self.redacted();
            }
            Expectation::None => {}
        }

        if let Some((name, separator)) = split_assignment(&normalized) {
            if sensitive_name(name) {
                if separator + 1 >= normalized.len() {
                    self.expectation = Expectation::Value;
                    return token;
                }
                let original_separator = token
                    .char_indices()
                    .find(|(_, character)| matches!(character, '=' | ':'))
                    .map(|(index, _)| index)
                    .unwrap_or(token.len());
                self.redacted_count = self.redacted_count.saturating_add(1);
                return format!(
                    "{}{}[REDACTED]",
                    &token[..original_separator],
                    &token[original_separator..=original_separator]
                );
            }
        }
        if sensitive_name(normalized.trim_end_matches([':', '='])) {
            self.expectation = Expectation::SeparatorOrValue;
            return token;
        }
        if looks_like_secret(&normalized) {
            return self.redacted();
        }
        token
    }

    fn redacted(&mut self) -> String {
        self.redacted_count = self.redacted_count.saturating_add(1);
        "[REDACTED]".into()
    }
}

pub fn redact_complete(value: &str) -> String {
    let mut redactor = OutputRedactor::default();
    let mut output = redactor.push(value);
    output.push_str(&redactor.finish());
    output
}

fn split_assignment(token: &str) -> Option<(&str, usize)> {
    token
        .char_indices()
        .find(|(_, character)| matches!(character, '=' | ':'))
        .map(|(index, _)| (&token[..index], index))
}

fn sensitive_name(name: &str) -> bool {
    let compact = name.replace(['_', '-'], "");
    [
        "apikey",
        "token",
        "accesstoken",
        "refreshtoken",
        "secret",
        "clientsecret",
        "password",
        "passwd",
        "authorization",
        "cookie",
    ]
    .iter()
    .any(|candidate| compact == *candidate || compact.ends_with(candidate))
}

fn looks_like_secret(token: &str) -> bool {
    let trimmed = token.trim_matches(|character: char| {
        matches!(
            character,
            '"' | '\'' | '`' | ',' | ';' | '(' | ')' | '[' | ']'
        )
    });
    (trimmed.starts_with("sk-") && trimmed.len() >= 20)
        || (trimmed.starts_with("ghp_") && trimmed.len() >= 20)
        || (trimmed.starts_with("github_pat_") && trimmed.len() >= 24)
        || (trimmed.starts_with("eyj") && trimmed.matches('.').count() == 2 && trimmed.len() >= 32)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn redact(chunks: &[&str]) -> (String, OutputRedactor) {
        let mut redactor = OutputRedactor::default();
        let mut output = String::new();
        for chunk in chunks {
            output.push_str(&redactor.push(chunk));
        }
        output.push_str(&redactor.finish());
        (output, redactor)
    }

    #[test]
    fn redacts_assignments_authorization_and_known_token_shapes() {
        let (output, redactor) = redact(&[
            "OPENAI_API_KEY=sk-12345678901234567890\n",
            "Authorization: Bearer eyJheader.payload.signature\n",
            "password = swordfish\n",
            "github_pat_123456789012345678901234\n",
        ]);
        assert!(!output.contains("12345678901234567890"));
        assert!(!output.contains("swordfish"));
        assert!(!output.contains("signature"));
        assert!(output.contains("OPENAI_API_KEY=[REDACTED]"));
        assert!(output.contains("Authorization: Bearer [REDACTED]"));
        assert!(redactor.redacted_count() >= 4);
    }

    #[test]
    fn redacts_values_split_across_stream_chunks() {
        let (output, redactor) = redact(&["API_", "KEY = ", "sk-abcdefghij", "klmnopqrst\n"]);
        assert_eq!(output, "API_KEY = [REDACTED]\n");
        assert_eq!(redactor.redacted_count(), 1);
        assert_eq!(redactor.source_bytes(), 34);
    }

    #[test]
    fn ordinary_unicode_output_is_preserved() {
        let (output, redactor) = redact(&["测试通过 ", "42\n"]);
        assert_eq!(output, "测试通过 42\n");
        assert_eq!(redactor.redacted_count(), 0);
    }

    #[test]
    fn long_sensitive_tokens_are_redacted_without_unbounded_buffering() {
        let secret = format!("API_KEY={}", "s".repeat(32 * 1024));
        let (output, redactor) = redact(&[&secret]);
        assert_eq!(output, "API_KEY=[REDACTED]");
        assert_eq!(redactor.redacted_count(), 1);
        assert_eq!(redactor.source_bytes(), secret.len() as u64);
    }

    #[test]
    fn finish_redacts_an_unterminated_authorization_value() {
        let secret = "ghp_123456789012345678901234567890";
        let (output, redactor) = redact(&["Authorization: Bearer ", secret]);
        assert_eq!(output, "Authorization: Bearer [REDACTED]");
        assert_eq!(redactor.redacted_count(), 1);
        assert!(!output.contains(secret));
    }

    #[test]
    fn complete_redaction_sanitizes_command_style_flags() {
        let secret = "ghp_123456789012345678901234567890";
        let output = redact_complete(&format!("tool --token {secret} --verbose"));
        assert_eq!(output, "tool --token [REDACTED] --verbose");
        assert!(!output.contains(secret));
    }
}
