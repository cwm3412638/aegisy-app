const CURSOR_POSITION_QUERY: &[u8] = b"\x1b[6n";

#[derive(Debug, Default)]
pub(crate) struct CursorPositionQueryTracker {
    next_output_offset: u64,
    trailing_bytes: Vec<u8>,
}

impl CursorPositionQueryTracker {
    pub(crate) fn observe(
        &mut self,
        output_start: u64,
        output_end: u64,
        output: &[u8],
    ) -> Result<usize, &'static str> {
        let output_bytes = u64::try_from(output.len())
            .map_err(|_| "terminal snapshot output length exceeds u64")?;
        if output_start.checked_add(output_bytes) != Some(output_end) {
            return Err("terminal snapshot output range does not match its bytes");
        }
        if self.next_output_offset < output_start {
            return Err("terminal snapshot omitted unobserved cursor queries");
        }
        if self.next_output_offset > output_end {
            return Err("terminal snapshot output range moved backwards");
        }

        let new_output_start = usize::try_from(self.next_output_offset - output_start)
            .map_err(|_| "terminal snapshot output offset exceeds usize")?;
        let new_output = &output[new_output_start..];
        let previous_tail_len = self.trailing_bytes.len();
        let mut scan = Vec::with_capacity(previous_tail_len.saturating_add(new_output.len()));
        scan.extend_from_slice(&self.trailing_bytes);
        scan.extend_from_slice(new_output);

        let query_count = scan
            .windows(CURSOR_POSITION_QUERY.len())
            .enumerate()
            .filter(|(start, window)| {
                *window == CURSOR_POSITION_QUERY
                    && start.saturating_add(CURSOR_POSITION_QUERY.len()) > previous_tail_len
            })
            .count();

        let trailing_start = scan.len().saturating_sub(CURSOR_POSITION_QUERY.len() - 1);
        self.trailing_bytes.clear();
        self.trailing_bytes
            .extend_from_slice(&scan[trailing_start..]);
        self.next_output_offset = output_end;
        Ok(query_count)
    }
}

pub(crate) fn contains_non_reset_sgr_wrapped_marker(output: &[u8], marker: &[u8]) -> bool {
    if marker.is_empty() {
        return false;
    }

    output
        .windows(marker.len())
        .enumerate()
        .filter(|(_, window)| *window == marker)
        .any(|(marker_start, _)| {
            let marker_end = marker_start + marker.len();
            has_adjacent_non_reset_sgr(&output[..marker_start])
                && has_adjacent_reset_sgr(&output[marker_end..])
        })
}

fn has_adjacent_non_reset_sgr(prefix: &[u8]) -> bool {
    let Some(sequence_start) = prefix.iter().rposition(|byte| *byte == 0x1b) else {
        return false;
    };
    let sequence = &prefix[sequence_start..];
    if sequence.len() < 4 || sequence[1] != b'[' || sequence.last() != Some(&b'm') {
        return false;
    }

    let parameters = &sequence[2..sequence.len() - 1];
    !parameters.is_empty()
        && parameters
            .split(|byte| *byte == b';')
            .all(|parameter| !parameter.is_empty() && parameter.iter().all(u8::is_ascii_digit))
        && parameters
            .split(|byte| *byte == b';')
            .any(|parameter| parameter.iter().any(|byte| *byte != b'0'))
}

fn has_adjacent_reset_sgr(suffix: &[u8]) -> bool {
    let suffix = suffix.strip_prefix(b"\r").unwrap_or(suffix);
    suffix.starts_with(b"\x1b[m") || suffix.starts_with(b"\x1b[0m")
}

#[cfg(test)]
mod tests {
    use super::*;

    const MARKER: &[u8] = b"AEGISY_ANSI_AFTER_INTERRUPT";

    #[test]
    fn cursor_query_tracker_counts_bursts_once_across_repeated_snapshots() {
        let mut tracker = CursorPositionQueryTracker::default();
        let first = b"prompt\x1b[6nnoise\x1b[6n";
        assert_eq!(tracker.observe(0, first.len() as u64, first).unwrap(), 2);
        assert_eq!(tracker.observe(0, first.len() as u64, first).unwrap(), 0);

        let second = b"prompt\x1b[6nnoise\x1b[6nnext\x1b[6n";
        assert_eq!(tracker.observe(0, second.len() as u64, second).unwrap(), 1);
    }

    #[test]
    fn cursor_query_tracker_detects_every_split_across_snapshots() {
        for split in 1..CURSOR_POSITION_QUERY.len() {
            let mut tracker = CursorPositionQueryTracker::default();
            let first = &CURSOR_POSITION_QUERY[..split];
            assert_eq!(tracker.observe(0, split as u64, first).unwrap(), 0);

            let complete = CURSOR_POSITION_QUERY;
            assert_eq!(
                tracker.observe(0, complete.len() as u64, complete).unwrap(),
                1
            );
            assert_eq!(
                tracker.observe(0, complete.len() as u64, complete).unwrap(),
                0
            );
        }
    }

    #[test]
    fn cursor_query_tracker_uses_tail_at_an_exact_capture_boundary() {
        let mut tracker = CursorPositionQueryTracker::default();
        assert_eq!(tracker.observe(0, 3, b"\x1b[6").unwrap(), 0);
        assert_eq!(tracker.observe(3, 4, b"n").unwrap(), 1);
        assert_eq!(tracker.observe(3, 4, b"n").unwrap(), 0);
    }

    #[test]
    fn cursor_query_tracker_rejects_unobserved_or_inconsistent_ranges() {
        let mut omitted = CursorPositionQueryTracker::default();
        assert_eq!(
            omitted.observe(4, 8, b"data"),
            Err("terminal snapshot omitted unobserved cursor queries")
        );

        let mut inconsistent = CursorPositionQueryTracker::default();
        assert_eq!(
            inconsistent.observe(0, 5, b"data"),
            Err("terminal snapshot output range does not match its bytes")
        );

        let mut rewound = CursorPositionQueryTracker::default();
        rewound.observe(0, 4, b"data").unwrap();
        assert_eq!(
            rewound.observe(0, 3, b"dat"),
            Err("terminal snapshot output range moved backwards")
        );
    }

    #[test]
    fn accepts_explicit_and_conpty_normalized_sgr_resets() {
        assert!(contains_non_reset_sgr_wrapped_marker(
            b"before \x1b[31mAEGISY_ANSI_AFTER_INTERRUPT\x1b[0m after",
            MARKER,
        ));
        assert!(contains_non_reset_sgr_wrapped_marker(
            b"\x1b[31mAEGISY_ANSI_AFTER_INTERRUPT\r\x1b[m\n",
            MARKER,
        ));
        assert!(contains_non_reset_sgr_wrapped_marker(
            b"\x1b[1;31mAEGISY_ANSI_AFTER_INTERRUPT\x1b[m",
            MARKER,
        ));
    }

    #[test]
    fn rejects_marker_without_an_adjacent_non_reset_sgr_and_reset() {
        assert!(!contains_non_reset_sgr_wrapped_marker(MARKER, MARKER));
        assert!(!contains_non_reset_sgr_wrapped_marker(
            b"\x1b[31mother\x1b[0m AEGISY_ANSI_AFTER_INTERRUPT",
            MARKER,
        ));
        assert!(!contains_non_reset_sgr_wrapped_marker(
            b"\x1b[mAEGISY_ANSI_AFTER_INTERRUPT\x1b[0m",
            MARKER,
        ));
        assert!(!contains_non_reset_sgr_wrapped_marker(
            b"\x1b[31mAEGISY_ANSI_AFTER_INTERRUPT\x1b[1m",
            MARKER,
        ));
        assert!(!contains_non_reset_sgr_wrapped_marker(
            b"\x1b[31mAEGISY_ANSI_AFTER_INTERRUPT\r\r\x1b[m",
            MARKER,
        ));
    }
}
