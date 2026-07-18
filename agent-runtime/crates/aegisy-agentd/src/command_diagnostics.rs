const MAX_DIAGNOSTICS: usize = 200;
const MAX_MESSAGE_BYTES: usize = 1_000;
const MAX_PATH_BYTES: usize = 4 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParsedCommandDiagnostic {
    pub path: String,
    pub line: usize,
    pub column: usize,
    pub end_line: usize,
    pub end_column: usize,
    pub severity: String,
    pub code: Option<String>,
    pub message: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParsedCommandDiagnostics {
    pub toolchain: String,
    pub diagnostics: Vec<ParsedCommandDiagnostic>,
    pub truncated: bool,
}

pub fn parse(command: &str, output: &str) -> Option<ParsedCommandDiagnostics> {
    let toolchain = detect_toolchain(command)?;
    let output = strip_ansi(output);
    let mut diagnostics = Vec::new();
    let mut pending_rust = None;
    let mut eslint_path = None;
    let mut truncated = false;

    for line in output.lines() {
        if diagnostics.len() >= MAX_DIAGNOSTICS {
            truncated = true;
            break;
        }
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }

        if toolchain == "rustc" {
            if let Some((severity, code, message)) = pending_rust.take() {
                if let Some((path, line, column, _)) =
                    split_path_line_column(trimmed.trim_start_matches("--> "))
                {
                    push_diagnostic(
                        &mut diagnostics,
                        path,
                        line,
                        column,
                        severity,
                        code,
                        message,
                    );
                    continue;
                }
                pending_rust = Some((severity, code, message));
            }
            if let Some(header) = parse_rust_header(trimmed) {
                pending_rust = Some(header);
                continue;
            }
            if let Some(location) = trimmed.split_once("panicked at ").map(|(_, value)| value) {
                if let Some((path, line, column, _)) = split_path_line_column(location) {
                    push_diagnostic(
                        &mut diagnostics,
                        path,
                        line,
                        column,
                        "error".into(),
                        Some("panic".into()),
                        "Rust test panicked".into(),
                    );
                    continue;
                }
            }
        }

        if toolchain == "eslint" {
            if !line.starts_with(char::is_whitespace) && is_source_path(trimmed) {
                eslint_path = Some(trimmed.to_owned());
                continue;
            }
            if let Some(path) = eslint_path.as_deref() {
                if let Some((line, column, rest)) = split_line_column(trimmed) {
                    if let Some((severity, code, message)) = parse_eslint_detail(rest) {
                        push_diagnostic(
                            &mut diagnostics,
                            path,
                            line,
                            column,
                            severity,
                            code,
                            message,
                        );
                        continue;
                    }
                }
            }
        }

        if let Some((path, line, column, rest)) = split_parenthesized_location(trimmed) {
            if let Some((severity, code, message)) = parse_severity(rest) {
                push_diagnostic(
                    &mut diagnostics,
                    path,
                    line,
                    column,
                    severity,
                    code,
                    message,
                );
                continue;
            }
        }
        if let Some((path, line, column, rest)) = split_path_line_column(trimmed) {
            if let Some((severity, code, message)) =
                parse_severity(rest).or_else(|| parse_static_code(rest))
            {
                push_diagnostic(
                    &mut diagnostics,
                    path,
                    line,
                    column,
                    severity,
                    code,
                    message,
                );
                continue;
            }
        }
        if toolchain == "pytest" {
            if let Some((path, line, rest)) = split_path_line(trimmed) {
                if rest.contains("Error") || rest.contains("FAILED") || rest.contains("Failure") {
                    push_diagnostic(
                        &mut diagnostics,
                        path,
                        line,
                        1,
                        "error".into(),
                        None,
                        rest.trim().into(),
                    );
                }
            }
        }
    }

    Some(ParsedCommandDiagnostics {
        toolchain: toolchain.into(),
        diagnostics,
        truncated,
    })
}

fn detect_toolchain(command: &str) -> Option<&'static str> {
    let words = command
        .split(|character: char| character.is_whitespace() || ";&|()\"'".contains(character))
        .filter(|word| !word.is_empty())
        .map(|word| {
            word.rsplit(['/', '\\'])
                .next()
                .unwrap_or(word)
                .trim_end_matches(".exe")
                .to_ascii_lowercase()
        })
        .collect::<Vec<_>>();
    let has = |names: &[&str]| words.iter().any(|word| names.contains(&word.as_str()));
    if has(&["cargo", "rustc"]) {
        Some("rustc")
    } else if has(&["eslint"]) {
        Some("eslint")
    } else if has(&["ruff"]) {
        Some("ruff")
    } else if has(&["pytest"]) {
        Some("pytest")
    } else if has(&["mypy", "pyright"]) {
        Some("python-static")
    } else if has(&["tsc"]) {
        Some("typescript")
    } else if has(&[
        "clang", "clang++", "gcc", "g++", "cc", "c++", "cl", "cmake", "ninja", "make",
    ]) {
        Some("c-cpp")
    } else {
        None
    }
}

fn parse_rust_header(value: &str) -> Option<(String, Option<String>, String)> {
    for severity in ["error", "warning"] {
        let Some(rest) = value.strip_prefix(severity) else {
            continue;
        };
        if let Some(rest) = rest.strip_prefix('[') {
            let (code, message) = rest.split_once("]:")?;
            return Some((
                severity.into(),
                Some(bounded(code, 64)),
                bounded(message.trim(), MAX_MESSAGE_BYTES),
            ));
        }
        let message = rest.strip_prefix(':')?.trim();
        return Some((severity.into(), None, bounded(message, MAX_MESSAGE_BYTES)));
    }
    None
}

fn parse_severity(value: &str) -> Option<(String, Option<String>, String)> {
    let value = value.trim_start_matches([':', ' ']);
    for severity in ["error", "warning", "note", "help", "info"] {
        let Some(mut rest) = value.strip_prefix(severity) else {
            continue;
        };
        let normalized_severity = match severity {
            "note" | "help" => "hint",
            other => other,
        };
        rest = rest.trim_start();
        let mut code = None;
        if let Some(value) = rest.strip_prefix('[') {
            if let Some((candidate, tail)) = value.split_once(']') {
                code = Some(bounded(candidate, 64));
                rest = tail;
            }
        } else if !rest.starts_with(':') {
            if let Some((candidate, tail)) = rest.split_once(':') {
                if candidate.len() <= 32
                    && candidate
                        .chars()
                        .all(|character| character.is_ascii_alphanumeric() || character == '-')
                {
                    code = Some(candidate.trim().into());
                    rest = tail;
                }
            }
        }
        let message = rest.trim_start_matches([':', ' ']);
        if message.is_empty() {
            return None;
        }
        return Some((
            normalized_severity.into(),
            code,
            bounded(message, MAX_MESSAGE_BYTES),
        ));
    }
    None
}

fn parse_static_code(value: &str) -> Option<(String, Option<String>, String)> {
    let value = value.trim_start_matches([':', ' ']);
    let (code, message) = value.split_once(char::is_whitespace)?;
    if code.len() > 16
        || code.len() < 2
        || !code
            .chars()
            .all(|character| character.is_ascii_alphanumeric())
        || !code.chars().any(|character| character.is_ascii_digit())
    {
        return None;
    }
    let severity = if matches!(code.as_bytes()[0], b'E' | b'F') {
        "error"
    } else if code.as_bytes()[0] == b'W' {
        "warning"
    } else {
        "info"
    };
    Some((
        severity.into(),
        Some(code.into()),
        bounded(message.trim(), MAX_MESSAGE_BYTES),
    ))
}

fn parse_eslint_detail(value: &str) -> Option<(String, Option<String>, String)> {
    let mut parts = value.split_whitespace();
    let severity = parts.next()?;
    if !matches!(severity, "error" | "warning") {
        return None;
    }
    let rest = parts.collect::<Vec<_>>();
    if rest.is_empty() {
        return None;
    }
    let (message, code) = if rest.len() > 1 {
        (&rest[..rest.len() - 1], Some(rest[rest.len() - 1]))
    } else {
        (&rest[..], None)
    };
    Some((
        severity.into(),
        code.map(|value| bounded(value, 64)),
        bounded(&message.join(" "), MAX_MESSAGE_BYTES),
    ))
}

fn split_parenthesized_location(value: &str) -> Option<(&str, usize, usize, &str)> {
    let end = value.find("):")?;
    let start = value[..end].rfind('(')?;
    let path = value[..start].trim();
    let mut location = value[start + 1..end].split(',');
    let line = location.next()?.trim().parse().ok()?;
    let column = location.next().unwrap_or("1").trim().parse().ok()?;
    valid_location(path, line, column)?;
    Some((path, line, column, &value[end + 2..]))
}

fn split_path_line_column(value: &str) -> Option<(&str, usize, usize, &str)> {
    for (path_end, _) in value.match_indices(':') {
        let after_path = &value[path_end + 1..];
        let line_end = after_path.find(':')?;
        let Ok(line) = after_path[..line_end].trim().parse() else {
            continue;
        };
        let after_line = &after_path[line_end + 1..];
        let column_bytes = after_line.bytes().take_while(u8::is_ascii_digit).count();
        if column_bytes == 0 {
            continue;
        }
        let Ok(column) = after_line[..column_bytes].parse() else {
            continue;
        };
        let path = value[..path_end].trim();
        valid_location(path, line, column)?;
        let rest = after_line[column_bytes..].trim_start_matches(':');
        return Some((path, line, column, rest));
    }
    None
}

fn split_path_line(value: &str) -> Option<(&str, usize, &str)> {
    for (path_end, _) in value.match_indices(':') {
        let after_path = &value[path_end + 1..];
        let Some(line_end) = after_path.find(':') else {
            continue;
        };
        let Ok(line) = after_path[..line_end].trim().parse() else {
            continue;
        };
        let path = value[..path_end].trim();
        valid_location(path, line, 1)?;
        return Some((path, line, &after_path[line_end + 1..]));
    }
    None
}

fn split_line_column(value: &str) -> Option<(usize, usize, &str)> {
    let (line, rest) = value.split_once(':')?;
    let line = line.trim().parse().ok()?;
    let column_bytes = rest.bytes().take_while(u8::is_ascii_digit).count();
    if column_bytes == 0 {
        return None;
    }
    let column = rest[..column_bytes].parse().ok()?;
    valid_location("eslint", line, column)?;
    Some((line, column, rest[column_bytes..].trim()))
}

fn valid_location(path: &str, line: usize, column: usize) -> Option<()> {
    if path.is_empty()
        || path.len() > MAX_PATH_BYTES
        || path.chars().any(char::is_control)
        || line == 0
        || column == 0
        || line > 10_000_000
        || column > 10_000_000
    {
        return None;
    }
    Some(())
}

fn push_diagnostic(
    diagnostics: &mut Vec<ParsedCommandDiagnostic>,
    path: &str,
    line: usize,
    column: usize,
    severity: String,
    code: Option<String>,
    message: String,
) {
    diagnostics.push(ParsedCommandDiagnostic {
        path: bounded(path.trim_matches(['"', '\'']), MAX_PATH_BYTES),
        line,
        column,
        end_line: line,
        end_column: column.saturating_add(1),
        severity,
        code,
        message,
    });
}

fn is_source_path(value: &str) -> bool {
    let lower = value.to_ascii_lowercase();
    [
        ".js", ".jsx", ".mjs", ".cjs", ".ts", ".tsx", ".vue", ".py", ".rs", ".c", ".cc", ".cpp",
        ".cxx", ".h", ".hpp",
    ]
    .iter()
    .any(|extension| lower.ends_with(extension))
}

fn bounded(value: &str, limit: usize) -> String {
    if value.len() <= limit {
        return value.into();
    }
    let mut end = limit;
    while end > 0 && !value.is_char_boundary(end) {
        end -= 1;
    }
    value[..end].into()
}

fn strip_ansi(value: &str) -> String {
    let mut result = String::with_capacity(value.len());
    let mut characters = value.chars().peekable();
    while let Some(character) = characters.next() {
        if character != '\u{1b}' {
            result.push(character);
            continue;
        }
        if characters.next_if_eq(&'[').is_some() {
            for next in characters.by_ref() {
                if ('@'..='~').contains(&next) {
                    break;
                }
            }
        }
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_rustc_headers_and_locations_with_ansi() {
        let output =
            "\u{1b}[31merror[E0425]\u{1b}[0m: cannot find value `missing`\n --> src/main.rs:3:9\n";
        let parsed = parse("cargo check", output).unwrap();
        assert_eq!(parsed.toolchain, "rustc");
        assert_eq!(parsed.diagnostics.len(), 1);
        assert_eq!(parsed.diagnostics[0].path, "src/main.rs");
        assert_eq!(parsed.diagnostics[0].code.as_deref(), Some("E0425"));
        assert!(parsed.diagnostics[0].message.contains("cannot find value"));
    }

    #[test]
    fn parses_clang_typescript_and_msvc_locations() {
        let clang = parse(
            "cmake --build build",
            "src/main.cpp:4:7: error: use of undeclared identifier 'x' [-Werror]\n",
        )
        .unwrap();
        assert_eq!(clang.diagnostics[0].line, 4);
        let typescript = parse(
            "npx tsc --noEmit",
            "src/index.ts(8,12): error TS2304: Cannot find name 'value'.\n",
        )
        .unwrap();
        assert_eq!(typescript.diagnostics[0].code.as_deref(), Some("TS2304"));
        let msvc = parse(
            "cl /c src\\main.cpp",
            "src\\main.cpp(9,3): warning C4101: 'x': unreferenced local variable\n",
        )
        .unwrap();
        assert_eq!(msvc.diagnostics[0].severity, "warning");
        assert_eq!(msvc.diagnostics[0].code.as_deref(), Some("C4101"));
    }

    #[test]
    fn parses_ruff_eslint_and_pytest_without_guessing_unknown_commands() {
        let ruff = parse("ruff check .", "src/app.py:2:4: F821 Undefined name `x`\n").unwrap();
        assert_eq!(ruff.diagnostics[0].code.as_deref(), Some("F821"));
        let eslint = parse(
            "eslint src",
            "src/app.js\n  5:10  error  'missing' is not defined  no-undef\n",
        )
        .unwrap();
        assert_eq!(eslint.diagnostics[0].path, "src/app.js");
        assert_eq!(eslint.diagnostics[0].code.as_deref(), Some("no-undef"));
        let pytest = parse("pytest -q", "tests/test_app.py:7: AssertionError\n").unwrap();
        assert_eq!(pytest.diagnostics[0].line, 7);
        assert!(parse("printf diagnostics", "src/app.py:2:4: F821 missing\n").is_none());
    }

    #[test]
    fn bounds_diagnostic_count_and_messages() {
        let output = (0..300)
            .map(|index| format!("src/main.cpp:{index}:1: error: {}\n", "x".repeat(2_000)))
            .collect::<String>();
        let parsed = parse("clang src/main.cpp", &output).unwrap();
        assert_eq!(parsed.diagnostics.len(), MAX_DIAGNOSTICS);
        assert!(parsed.truncated);
        assert!(parsed
            .diagnostics
            .iter()
            .all(|item| item.message.len() <= MAX_MESSAGE_BYTES));
    }
}
