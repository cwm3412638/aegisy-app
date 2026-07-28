use serde::Serialize;

pub const INLINE_HEAD_LIMIT: usize = 64 * 1024;
pub const INLINE_TAIL_LIMIT: usize = 192 * 1024;
pub const ARTIFACT_HEAD_LIMIT: usize = 1024 * 1024;
pub const ARTIFACT_TAIL_LIMIT: usize = 1024 * 1024;

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct CommandOutputCapture {
    inline_head: String,
    inline_tail: String,
    artifact_head: String,
    artifact_tail: String,
    total_bytes: u64,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct CommandOutputSnapshot {
    pub head: String,
    pub tail: String,
    pub total_bytes: u64,
    pub retained_bytes: usize,
    pub omitted_bytes: u64,
    pub truncated: bool,
    pub head_limit: usize,
    pub tail_limit: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CommandOutputArtifactCapture {
    pub content: String,
    pub total_bytes: u64,
    pub retained_bytes: usize,
    pub omitted_bytes: u64,
    pub truncated: bool,
}

impl CommandOutputCapture {
    pub fn append(&mut self, delta: &str) {
        self.total_bytes = self.total_bytes.saturating_add(delta.len() as u64);
        append_prefix(&mut self.inline_head, delta, INLINE_HEAD_LIMIT);
        append_tail(&mut self.inline_tail, delta, INLINE_TAIL_LIMIT);
        append_prefix(&mut self.artifact_head, delta, ARTIFACT_HEAD_LIMIT);
        append_tail(&mut self.artifact_tail, delta, ARTIFACT_TAIL_LIMIT);
    }

    pub fn snapshot(&self) -> CommandOutputSnapshot {
        let head_bytes = self.inline_head.len();
        let tail = if self.total_bytes <= head_bytes as u64 {
            String::new()
        } else {
            non_overlapping_tail(&self.inline_head, &self.inline_tail, self.total_bytes).to_owned()
        };
        let retained_bytes = head_bytes.saturating_add(tail.len());
        CommandOutputSnapshot {
            head: self.inline_head.clone(),
            tail,
            total_bytes: self.total_bytes,
            retained_bytes,
            omitted_bytes: self.total_bytes.saturating_sub(retained_bytes as u64),
            truncated: self.total_bytes > retained_bytes as u64,
            head_limit: INLINE_HEAD_LIMIT,
            tail_limit: INLINE_TAIL_LIMIT,
        }
    }

    pub fn artifact(&self) -> CommandOutputArtifactCapture {
        if self.total_bytes <= ARTIFACT_HEAD_LIMIT as u64 {
            return CommandOutputArtifactCapture {
                content: self.artifact_head.clone(),
                total_bytes: self.total_bytes,
                retained_bytes: self.artifact_head.len(),
                omitted_bytes: 0,
                truncated: false,
            };
        }
        let tail = non_overlapping_tail(&self.artifact_head, &self.artifact_tail, self.total_bytes);
        let retained_bytes = self.artifact_head.len().saturating_add(tail.len());
        let omitted_bytes = self.total_bytes.saturating_sub(retained_bytes as u64);
        let marker = if omitted_bytes > 0 {
            format!("\n[Aegisy omitted {omitted_bytes} command output bytes]\n")
        } else {
            String::new()
        };
        // Preserve a unique Runtime-owned omission boundary even when command
        // output deliberately contains the exact same marker text. The brace
        // substitution preserves byte length, so retained/source arithmetic
        // and UTF-8 boundaries remain unchanged.
        let escaped_marker = marker.replacen('[', "{", 1);
        let head = self.artifact_head.replace(&marker, &escaped_marker);
        let tail = tail.replace(&marker, &escaped_marker);
        CommandOutputArtifactCapture {
            content: format!("{head}{marker}{tail}"),
            total_bytes: self.total_bytes,
            retained_bytes,
            omitted_bytes,
            truncated: omitted_bytes > 0,
        }
    }
}

fn non_overlapping_tail<'a>(head: &str, tail: &'a str, total_bytes: u64) -> &'a str {
    let combined = head.len().saturating_add(tail.len()) as u64;
    let overlap = combined.saturating_sub(total_bytes) as usize;
    if overlap == 0 {
        return tail;
    }
    let mut start = overlap.min(tail.len());
    while start < tail.len() && !tail.is_char_boundary(start) {
        start += 1;
    }
    &tail[start..]
}

fn append_prefix(target: &mut String, delta: &str, limit: usize) {
    if target.len() >= limit {
        return;
    }
    let available = limit - target.len();
    target.push_str(prefix_bytes(delta, available));
}

fn append_tail(target: &mut String, delta: &str, limit: usize) {
    if delta.len() >= limit {
        target.clear();
        target.push_str(suffix_bytes(delta, limit));
        return;
    }
    target.push_str(delta);
    if target.len() > limit {
        let retained = suffix_bytes(target, limit).to_owned();
        *target = retained;
    }
}

fn prefix_bytes(value: &str, limit: usize) -> &str {
    if value.len() <= limit {
        return value;
    }
    let mut end = limit;
    while end > 0 && !value.is_char_boundary(end) {
        end -= 1;
    }
    &value[..end]
}

fn suffix_bytes(value: &str, limit: usize) -> &str {
    if value.len() <= limit {
        return value;
    }
    let mut start = value.len() - limit;
    while start < value.len() && !value.is_char_boundary(start) {
        start += 1;
    }
    &value[start..]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn small_output_is_not_duplicated_or_truncated() {
        let mut capture = CommandOutputCapture::default();
        capture.append("hello\n");
        let snapshot = capture.snapshot();
        assert_eq!(snapshot.head, "hello\n");
        assert!(snapshot.tail.is_empty());
        assert_eq!(snapshot.total_bytes, 6);
        assert_eq!(snapshot.omitted_bytes, 0);
        assert!(!snapshot.truncated);
        let artifact = capture.artifact();
        assert_eq!(artifact.content, "hello\n");
        assert!(!artifact.truncated);
    }

    #[test]
    fn huge_unicode_output_keeps_bounded_head_tail_and_artifact() {
        let mut capture = CommandOutputCapture::default();
        let delta = "开".repeat(1024 * 1024);
        for _ in 0..4 {
            capture.append(&delta);
        }
        let snapshot = capture.snapshot();
        assert!(snapshot.head.len() <= INLINE_HEAD_LIMIT);
        assert!(snapshot.tail.len() <= INLINE_TAIL_LIMIT);
        assert!(snapshot.truncated);
        assert!(snapshot.omitted_bytes > 0);
        assert!(snapshot.head.is_char_boundary(snapshot.head.len()));
        assert!(snapshot.tail.is_char_boundary(0));
        let artifact = capture.artifact();
        assert!(artifact.retained_bytes <= ARTIFACT_HEAD_LIMIT + ARTIFACT_TAIL_LIMIT);
        assert!(artifact.content.len() <= ARTIFACT_HEAD_LIMIT + ARTIFACT_TAIL_LIMIT + 80);
        assert!(artifact.truncated);
        assert!(artifact.content.contains("Aegisy omitted"));
    }

    #[test]
    fn overlapping_head_and_tail_are_de_duplicated() {
        let output = "a".repeat(100 * 1024);
        let mut capture = CommandOutputCapture::default();
        capture.append(&output);
        let snapshot = capture.snapshot();
        assert_eq!(snapshot.head.len() + snapshot.tail.len(), output.len());
        assert_eq!(format!("{}{}", snapshot.head, snapshot.tail), output);
        assert_eq!(snapshot.omitted_bytes, 0);
        assert!(!snapshot.truncated);

        let output = "b".repeat(1500 * 1024);
        let mut capture = CommandOutputCapture::default();
        capture.append(&output);
        let artifact = capture.artifact();
        assert_eq!(artifact.content, output);
        assert_eq!(artifact.omitted_bytes, 0);
        assert!(!artifact.truncated);
    }

    #[test]
    fn many_small_deltas_never_exceed_capture_limits() {
        let mut capture = CommandOutputCapture::default();
        for _ in 0..100_000 {
            capture.append("1234567890\n");
        }
        let snapshot = capture.snapshot();
        let artifact = capture.artifact();
        assert!(snapshot.head.len() <= INLINE_HEAD_LIMIT);
        assert!(snapshot.tail.len() <= INLINE_TAIL_LIMIT);
        assert!(artifact.retained_bytes <= ARTIFACT_HEAD_LIMIT + ARTIFACT_TAIL_LIMIT);
        assert_eq!(snapshot.total_bytes, 1_100_000);
    }

    #[test]
    fn source_text_equal_to_the_omission_marker_is_escaped_without_changing_counts() {
        let omitted_bytes = 1024 * 1024;
        let marker = format!("\n[Aegisy omitted {omitted_bytes} command output bytes]\n");
        let head = format!("{marker}{}", "h".repeat(ARTIFACT_HEAD_LIMIT - marker.len()));
        let middle = "m".repeat(omitted_bytes);
        let tail = "t".repeat(ARTIFACT_TAIL_LIMIT);
        let mut capture = CommandOutputCapture::default();
        capture.append(&format!("{head}{middle}{tail}"));

        let artifact = capture.artifact();
        assert_eq!(artifact.omitted_bytes, omitted_bytes as u64);
        assert_eq!(
            artifact.retained_bytes,
            ARTIFACT_HEAD_LIMIT + ARTIFACT_TAIL_LIMIT
        );
        assert_eq!(artifact.content.match_indices(&marker).count(), 1);
        assert!(artifact.content.contains(&marker.replacen('[', "{", 1)));
        assert_eq!(
            artifact.content.len(),
            artifact.retained_bytes + marker.len()
        );
    }
}
