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
