use crate::workspace::{read_text_file, SearchCandidate, WorkspaceError};
use serde::Serialize;
use std::collections::{BTreeMap, HashSet};
use std::path::Path;
use tree_sitter::{Language, Parser, Query, QueryCursor, StreamingIterator};

pub const MAX_INDEX_BYTES: u64 = 8 * 1024 * 1024;
pub const MAX_INDEX_SYMBOLS: usize = 20_000;
pub const MAX_INDEX_DEPENDENCIES: usize = 10_000;
pub const MIN_REPOSITORY_MAP_TOKENS: usize = 256;
pub const MAX_REPOSITORY_MAP_TOKENS: usize = 8_192;

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct RepositorySymbol {
    pub path: String,
    pub name: String,
    pub kind: String,
    pub line: usize,
    pub column: usize,
    pub end_line: usize,
    pub end_column: usize,
    pub language: String,
    pub revision: String,
    pub provenance: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct DependencyEdge {
    pub from: String,
    pub target: String,
    pub kind: String,
    pub line: usize,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceIndexResult {
    pub snapshot: String,
    pub stale: bool,
    pub indexed_files: usize,
    pub reused_files: usize,
    pub parsed_files: usize,
    pub skipped_files: usize,
    pub truncated: bool,
    pub cancelled: bool,
    pub languages: BTreeMap<String, usize>,
    pub symbols: Vec<RepositorySymbol>,
    pub dependencies: Vec<DependencyEdge>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct RepositoryMapResult {
    pub snapshot: String,
    pub stale: bool,
    pub token_budget: usize,
    pub estimated_tokens: usize,
    pub truncated: bool,
    pub included_files: Vec<String>,
    pub text: String,
}

#[derive(Debug, Clone, Default)]
pub struct WorkspaceIndex {
    files: BTreeMap<String, IndexedFile>,
    snapshot: String,
    stale: bool,
    truncated: bool,
}

#[derive(Debug, Clone)]
struct IndexedFile {
    revision: String,
    language: String,
    symbols: Vec<RepositorySymbol>,
    dependencies: Vec<DependencyEdge>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SourceLanguage {
    Rust,
    Python,
    JavaScript,
    TypeScript,
    Tsx,
    Cpp,
}

impl WorkspaceIndex {
    pub fn mark_stale(&mut self) {
        self.stale = true;
    }

    pub fn cancelled_result(&self) -> WorkspaceIndexResult {
        let mut result = self.result(0, 0, 0);
        result.cancelled = true;
        result
    }

    pub fn refresh(
        &mut self,
        root: &Path,
        candidates: &[SearchCandidate],
        ignored: &HashSet<String>,
        candidate_truncated: bool,
    ) -> Result<WorkspaceIndexResult, WorkspaceError> {
        self.refresh_cancellable(root, candidates, ignored, candidate_truncated, || false)
    }

    pub fn refresh_cancellable<F>(
        &mut self,
        root: &Path,
        candidates: &[SearchCandidate],
        ignored: &HashSet<String>,
        candidate_truncated: bool,
        mut should_cancel: F,
    ) -> Result<WorkspaceIndexResult, WorkspaceError>
    where
        F: FnMut() -> bool,
    {
        let mut next_files = BTreeMap::new();
        let mut parsed_files = 0;
        let mut reused_files = 0;
        let mut skipped_files = 0;
        let mut parsed_bytes = 0_u64;
        let mut symbol_count = 0_usize;
        let mut dependency_count = 0_usize;
        let mut truncated = candidate_truncated;

        for candidate in candidates {
            if should_cancel() {
                let mut result = self.result(0, 0, 0);
                result.cancelled = true;
                return Ok(result);
            }
            if ignored.contains(&candidate.path) {
                continue;
            }
            let Some(language) = SourceLanguage::from_path(&candidate.path) else {
                continue;
            };
            if let Some(existing) = self.files.get(&candidate.path) {
                if existing.revision == candidate.revision {
                    if symbol_count.saturating_add(existing.symbols.len()) > MAX_INDEX_SYMBOLS
                        || dependency_count.saturating_add(existing.dependencies.len())
                            > MAX_INDEX_DEPENDENCIES
                    {
                        skipped_files += 1;
                        truncated = true;
                        continue;
                    }
                    symbol_count += existing.symbols.len();
                    dependency_count += existing.dependencies.len();
                    next_files.insert(candidate.path.clone(), existing.clone());
                    reused_files += 1;
                    continue;
                }
            }
            if candidate.size > crate::workspace::MAX_TEXT_FILE_BYTES
                || parsed_bytes.saturating_add(candidate.size) > MAX_INDEX_BYTES
            {
                skipped_files += 1;
                truncated = true;
                continue;
            }
            let file = match read_text_file(root, &candidate.path) {
                Ok(file) => file,
                Err(error) if matches!(error.code, -32036..=-32033) => {
                    skipped_files += 1;
                    continue;
                }
                Err(error) => return Err(error),
            };
            parsed_bytes = parsed_bytes.saturating_add(file.size);
            let (mut symbols, mut dependencies) = parse_source(
                language,
                &candidate.path,
                &candidate.revision,
                &file.content,
            )?;
            let remaining_symbols = MAX_INDEX_SYMBOLS.saturating_sub(symbol_count);
            if symbols.len() > remaining_symbols {
                symbols.truncate(remaining_symbols);
                truncated = true;
            }
            let remaining_dependencies = MAX_INDEX_DEPENDENCIES.saturating_sub(dependency_count);
            if dependencies.len() > remaining_dependencies {
                dependencies.truncate(remaining_dependencies);
                truncated = true;
            }
            symbol_count += symbols.len();
            dependency_count += dependencies.len();
            next_files.insert(
                candidate.path.clone(),
                IndexedFile {
                    revision: candidate.revision.clone(),
                    language: language.name().into(),
                    symbols,
                    dependencies,
                },
            );
            parsed_files += 1;
        }

        if should_cancel() {
            let mut result = self.result(0, 0, 0);
            result.cancelled = true;
            return Ok(result);
        }

        self.files = next_files;
        self.snapshot = index_snapshot(&self.files);
        self.stale = false;
        self.truncated = truncated;
        Ok(self.result(parsed_files, reused_files, skipped_files))
    }

    pub fn repository_map(
        &self,
        token_budget: usize,
        focus_paths: &[String],
    ) -> RepositoryMapResult {
        let token_budget = token_budget.clamp(MIN_REPOSITORY_MAP_TOKENS, MAX_REPOSITORY_MAP_TOKENS);
        let character_budget = token_budget.saturating_mul(4);
        let focus = focus_paths.iter().collect::<HashSet<_>>();
        let mut ranked = self.files.iter().collect::<Vec<_>>();
        ranked.sort_by(|(left_path, left), (right_path, right)| {
            let left_focus = usize::from(focus.contains(left_path));
            let right_focus = usize::from(focus.contains(right_path));
            right_focus
                .cmp(&left_focus)
                .then_with(|| right.symbols.len().cmp(&left.symbols.len()))
                .then_with(|| right.dependencies.len().cmp(&left.dependencies.len()))
                .then_with(|| left_path.cmp(right_path))
        });

        let mut text = String::from("# Aegisy repository map\n");
        let mut included_files = Vec::new();
        let mut truncated = self.truncated;
        for (path, file) in ranked {
            let mut section = format!("\n{path} [{}]\n", file.language);
            for dependency in &file.dependencies {
                section.push_str(&format!(
                    "  -> {} ({}, line {})\n",
                    dependency.target, dependency.kind, dependency.line
                ));
            }
            for symbol in &file.symbols {
                section.push_str(&format!(
                    "  {} {} ({}:{})\n",
                    symbol.kind, symbol.name, symbol.line, symbol.column
                ));
            }
            if text.chars().count().saturating_add(section.chars().count()) > character_budget {
                truncated = true;
                continue;
            }
            text.push_str(&section);
            included_files.push(path.clone());
        }
        let estimated_tokens = text.chars().count().div_ceil(4);
        RepositoryMapResult {
            snapshot: self.snapshot.clone(),
            stale: self.stale,
            token_budget,
            estimated_tokens,
            truncated,
            included_files,
            text,
        }
    }

    fn result(
        &self,
        parsed_files: usize,
        reused_files: usize,
        skipped_files: usize,
    ) -> WorkspaceIndexResult {
        let mut languages = BTreeMap::new();
        let mut symbols = Vec::new();
        let mut dependencies = Vec::new();
        for file in self.files.values() {
            *languages.entry(file.language.clone()).or_insert(0) += 1;
            symbols.extend(file.symbols.clone());
            dependencies.extend(file.dependencies.clone());
        }
        WorkspaceIndexResult {
            snapshot: self.snapshot.clone(),
            stale: self.stale,
            indexed_files: self.files.len(),
            reused_files,
            parsed_files,
            skipped_files,
            truncated: self.truncated,
            cancelled: false,
            languages,
            symbols,
            dependencies,
        }
    }
}

impl SourceLanguage {
    fn from_path(path: &str) -> Option<Self> {
        let extension = Path::new(path)
            .extension()
            .and_then(|extension| extension.to_str())?
            .to_ascii_lowercase();
        match extension.as_str() {
            "rs" => Some(Self::Rust),
            "py" | "pyi" => Some(Self::Python),
            "js" | "jsx" | "mjs" | "cjs" => Some(Self::JavaScript),
            "ts" | "mts" | "cts" => Some(Self::TypeScript),
            "tsx" => Some(Self::Tsx),
            "c" | "cc" | "cpp" | "cxx" | "h" | "hh" | "hpp" | "hxx" => Some(Self::Cpp),
            _ => None,
        }
    }

    const fn name(self) -> &'static str {
        match self {
            Self::Rust => "rust",
            Self::Python => "python",
            Self::JavaScript => "javascript",
            Self::TypeScript => "typescript",
            Self::Tsx => "tsx",
            Self::Cpp => "cpp",
        }
    }

    fn language(self) -> Language {
        match self {
            Self::Rust => tree_sitter_rust::LANGUAGE.into(),
            Self::Python => tree_sitter_python::LANGUAGE.into(),
            Self::JavaScript => tree_sitter_javascript::LANGUAGE.into(),
            Self::TypeScript => tree_sitter_typescript::LANGUAGE_TYPESCRIPT.into(),
            Self::Tsx => tree_sitter_typescript::LANGUAGE_TSX.into(),
            Self::Cpp => tree_sitter_cpp::LANGUAGE.into(),
        }
    }

    fn tags_query(self) -> String {
        match self {
            Self::Rust => tree_sitter_rust::TAGS_QUERY.into(),
            Self::Python => tree_sitter_python::TAGS_QUERY.into(),
            Self::JavaScript => tree_sitter_javascript::TAGS_QUERY.into(),
            Self::TypeScript | Self::Tsx => format!(
                "{}\n{}",
                tree_sitter_javascript::TAGS_QUERY,
                tree_sitter_typescript::TAGS_QUERY
            ),
            Self::Cpp => tree_sitter_cpp::TAGS_QUERY.into(),
        }
    }

    const fn dependency_query(self) -> &'static str {
        match self {
            Self::Rust => "[(use_declaration) (extern_crate_declaration)] @dependency",
            Self::Python => "[(import_statement) (import_from_statement)] @dependency",
            Self::JavaScript | Self::TypeScript | Self::Tsx => {
                "[(import_statement) (export_statement)] @dependency\n\
                 (call_expression function: (identifier) @function arguments: (arguments (string) @dependency) (#eq? @function \"require\"))"
            }
            Self::Cpp => "(preproc_include) @dependency",
        }
    }
}

fn parse_source(
    source_language: SourceLanguage,
    path: &str,
    revision: &str,
    source: &str,
) -> Result<(Vec<RepositorySymbol>, Vec<DependencyEdge>), WorkspaceError> {
    let language = source_language.language();
    let mut parser = Parser::new();
    parser.set_language(&language).map_err(|cause| {
        index_error(format!(
            "cannot load {} grammar: {cause}",
            source_language.name()
        ))
    })?;
    let tree = parser
        .parse(source, None)
        .ok_or_else(|| index_error("tree-sitter parser did not return a syntax tree"))?;

    let tag_query_source = source_language.tags_query();
    let tag_query = Query::new(&language, &tag_query_source).map_err(|cause| {
        index_error(format!(
            "invalid {} tag query: {cause}",
            source_language.name()
        ))
    })?;
    let capture_names = tag_query.capture_names();
    let mut cursor = QueryCursor::new();
    let mut matches = cursor.matches(&tag_query, tree.root_node(), source.as_bytes());
    let mut symbols = Vec::new();
    let mut seen_symbols = HashSet::new();
    while let Some(query_match) = matches.next() {
        let definition = query_match.captures.iter().find_map(|capture| {
            let capture_name = capture_names[capture.index as usize];
            capture_name
                .strip_prefix("definition.")
                .map(|kind| (capture.node, kind))
        });
        let name_node = query_match.captures.iter().find_map(|capture| {
            (capture_names[capture.index as usize] == "name").then_some(capture.node)
        });
        let (Some((definition_node, kind)), Some(name_node)) = (definition, name_node) else {
            continue;
        };
        let Ok(name) = name_node.utf8_text(source.as_bytes()) else {
            continue;
        };
        let name = name.trim();
        if name.is_empty() {
            continue;
        }
        let start = name_node.start_position();
        let end = definition_node.end_position();
        let identity = (name.to_owned(), kind.to_owned(), start.row, start.column);
        if !seen_symbols.insert(identity) {
            continue;
        }
        symbols.push(RepositorySymbol {
            path: path.into(),
            name: name.into(),
            kind: kind.into(),
            line: start.row + 1,
            column: start.column + 1,
            end_line: end.row + 1,
            end_column: end.column + 1,
            language: source_language.name().into(),
            revision: revision.into(),
            provenance: "tree-sitter".into(),
        });
    }
    drop(matches);

    let dependency_query =
        Query::new(&language, source_language.dependency_query()).map_err(|cause| {
            index_error(format!(
                "invalid {} dependency query: {cause}",
                source_language.name()
            ))
        })?;
    let dependency_names = dependency_query.capture_names();
    let mut dependency_cursor = QueryCursor::new();
    let mut dependency_matches =
        dependency_cursor.matches(&dependency_query, tree.root_node(), source.as_bytes());
    let mut dependencies = Vec::new();
    let mut seen_dependencies = HashSet::new();
    while let Some(query_match) = dependency_matches.next() {
        for capture in query_match.captures {
            if dependency_names[capture.index as usize] != "dependency" {
                continue;
            }
            let Ok(raw) = capture.node.utf8_text(source.as_bytes()) else {
                continue;
            };
            let Some(target) = normalize_dependency(source_language, raw) else {
                continue;
            };
            let line = capture.node.start_position().row + 1;
            if seen_dependencies.insert((target.clone(), line)) {
                dependencies.push(DependencyEdge {
                    from: path.into(),
                    target,
                    kind: if source_language == SourceLanguage::Cpp {
                        "include".into()
                    } else {
                        "import".into()
                    },
                    line,
                });
            }
        }
    }
    Ok((symbols, dependencies))
}

fn normalize_dependency(language: SourceLanguage, raw: &str) -> Option<String> {
    let raw = raw.trim();
    let target = match language {
        SourceLanguage::Rust => raw
            .strip_prefix("use ")
            .or_else(|| raw.strip_prefix("extern crate "))?
            .trim_end_matches(';')
            .trim(),
        SourceLanguage::Python => {
            if let Some(imports) = raw.strip_prefix("from ") {
                imports.split_once(" import ")?.0.trim()
            } else {
                raw.strip_prefix("import ")?.trim()
            }
        }
        SourceLanguage::JavaScript | SourceLanguage::TypeScript | SourceLanguage::Tsx => {
            quoted_value(raw)?
        }
        SourceLanguage::Cpp => raw
            .strip_prefix("#include")?
            .trim()
            .trim_matches(['<', '>', '\'', '"'])
            .trim(),
    };
    (!target.is_empty()).then(|| target.to_owned())
}

fn quoted_value(value: &str) -> Option<&str> {
    let (start, quote) = value
        .char_indices()
        .find(|(_, character)| matches!(character, '\'' | '"'))?;
    let rest = &value[start + quote.len_utf8()..];
    let end = rest.find(quote)?;
    Some(&rest[..end])
}

fn index_snapshot(files: &BTreeMap<String, IndexedFile>) -> String {
    let mut hash = 0xcbf29ce484222325_u64;
    for (path, file) in files {
        for byte in path.as_bytes().iter().chain(file.revision.as_bytes()) {
            hash ^= u64::from(*byte);
            hash = hash.wrapping_mul(0x100000001b3);
        }
    }
    format!("index:{hash:016x}:{}", files.len())
}

fn index_error(message: impl Into<String>) -> WorkspaceError {
    WorkspaceError {
        code: -32060,
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::workspace::collect_search_candidates;
    use std::fs;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{SystemTime, UNIX_EPOCH};

    static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

    fn fixture() -> std::path::PathBuf {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "aegisy-repository-index-{}-{unique}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(root.join("src")).unwrap();
        root
    }

    fn refresh(root: &Path, index: &mut WorkspaceIndex) -> WorkspaceIndexResult {
        let (candidates, truncated) = collect_search_candidates(root).unwrap();
        index
            .refresh(root, &candidates, &HashSet::new(), truncated)
            .unwrap()
    }

    #[test]
    fn extracts_symbols_and_dependencies_for_supported_languages() {
        let root = fixture();
        fs::write(
            root.join("src/lib.rs"),
            "use crate::tools;\npub struct Engine;\npub fn run() {}\n",
        )
        .unwrap();
        fs::write(
            root.join("src/tool.py"),
            "from app.core import Base\nclass Tool(Base):\n    def run(self):\n        pass\n",
        )
        .unwrap();
        fs::write(
            root.join("src/main.js"),
            "import api from './api.js';\nexport function start() {}\n",
        )
        .unwrap();
        fs::write(
            root.join("src/view.tsx"),
            "import React from 'react';\nexport function View() { return <div />; }\n",
        )
        .unwrap();
        fs::write(
            root.join("src/tool.cpp"),
            "#include <vector>\nclass Tool {};\nint run() { return 0; }\n",
        )
        .unwrap();

        let result = refresh(&root, &mut WorkspaceIndex::default());
        assert_eq!(result.indexed_files, 5);
        assert!(result
            .symbols
            .iter()
            .any(|symbol| symbol.name == "Engine" && symbol.kind == "class"));
        assert!(result
            .symbols
            .iter()
            .any(|symbol| symbol.name == "Tool" && symbol.language == "python"));
        assert!(result
            .symbols
            .iter()
            .any(|symbol| symbol.name == "View" && symbol.language == "tsx"));
        assert!(result.symbols.iter().all(|symbol| {
            symbol.line > 0
                && symbol.column > 0
                && symbol.end_line >= symbol.line
                && symbol.provenance == "tree-sitter"
        }));
        assert!(result
            .dependencies
            .iter()
            .any(|edge| edge.target == "crate::tools"));
        assert!(result
            .dependencies
            .iter()
            .any(|edge| edge.target == "app.core"));
        assert!(result
            .dependencies
            .iter()
            .any(|edge| edge.target == "react"));
        assert!(result
            .dependencies
            .iter()
            .any(|edge| edge.target == "vector" && edge.kind == "include"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn reuses_unchanged_files_and_removes_deleted_files() {
        let root = fixture();
        fs::write(root.join("src/one.rs"), "pub fn one() {}\n").unwrap();
        fs::write(root.join("src/two.rs"), "pub fn two() {}\n").unwrap();
        let mut index = WorkspaceIndex::default();
        let first = refresh(&root, &mut index);
        assert_eq!(first.parsed_files, 2);

        let second = refresh(&root, &mut index);
        assert_eq!(second.parsed_files, 0);
        assert_eq!(second.reused_files, 2);

        fs::write(root.join("src/one.rs"), "pub fn changed() { }\n").unwrap();
        fs::remove_file(root.join("src/two.rs")).unwrap();
        let third = refresh(&root, &mut index);
        assert_eq!(third.parsed_files, 1);
        assert_eq!(third.indexed_files, 1);
        assert!(third.symbols.iter().any(|symbol| symbol.name == "changed"));
        assert!(!third.symbols.iter().any(|symbol| symbol.name == "two"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn cancellation_preserves_the_last_complete_snapshot() {
        let root = fixture();
        for file_index in 0..12 {
            fs::write(
                root.join(format!("src/file_{file_index}.rs")),
                format!("pub fn symbol_{file_index}() {{}}\n"),
            )
            .unwrap();
        }
        let (candidates, truncated) = collect_search_candidates(&root).unwrap();
        let mut index = WorkspaceIndex::default();
        let first = index
            .refresh(&root, &candidates, &HashSet::new(), truncated)
            .unwrap();
        let snapshot = first.snapshot.clone();
        fs::write(root.join("src/new.rs"), "pub fn new_symbol() {}\n").unwrap();
        let (changed_candidates, changed_truncated) = collect_search_candidates(&root).unwrap();
        let mut checks = 0;
        let cancelled = index
            .refresh_cancellable(
                &root,
                &changed_candidates,
                &HashSet::new(),
                changed_truncated,
                || {
                    checks += 1;
                    checks > 3
                },
            )
            .unwrap();
        assert!(cancelled.cancelled);
        assert_eq!(cancelled.snapshot, snapshot);
        assert_eq!(cancelled.indexed_files, 12);
        assert!(!cancelled
            .symbols
            .iter()
            .any(|symbol| symbol.name == "new_symbol"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn huge_repository_limits_candidates_and_symbols() {
        let root = fixture();
        for file_index in 0..=crate::workspace::MAX_SEARCH_FILES {
            fs::write(root.join(format!("src/data_{file_index:05}.txt")), "x\n").unwrap();
        }
        let (candidates, candidate_truncated) = collect_search_candidates(&root).unwrap();
        assert_eq!(candidates.len(), crate::workspace::MAX_SEARCH_FILES);
        assert!(candidate_truncated);
        fs::remove_dir_all(&root).unwrap();

        let symbol_root = fixture();
        let mut source = String::new();
        for symbol_index in 0..=MAX_INDEX_SYMBOLS {
            source.push_str(&format!("fn s{symbol_index}() {{}}\n"));
        }
        assert!(source.len() < crate::workspace::MAX_TEXT_FILE_BYTES as usize);
        fs::write(symbol_root.join("src/huge.rs"), source).unwrap();
        let result = refresh(&symbol_root, &mut WorkspaceIndex::default());
        assert_eq!(result.symbols.len(), MAX_INDEX_SYMBOLS);
        assert!(result.truncated);
        fs::remove_dir_all(symbol_root).unwrap();
    }

    #[test]
    fn excludes_sensitive_symlinked_and_explicitly_ignored_files() {
        let root = fixture();
        fs::write(root.join("src/visible.rs"), "pub fn visible() {}\n").unwrap();
        fs::write(root.join(".env"), "SECRET=value\n").unwrap();
        #[cfg(unix)]
        std::os::unix::fs::symlink(root.join("src/visible.rs"), root.join("src/link.rs")).unwrap();
        let (candidates, truncated) = collect_search_candidates(&root).unwrap();
        assert!(!candidates.iter().any(|candidate| candidate.path == ".env"));
        assert!(!candidates
            .iter()
            .any(|candidate| candidate.path == "src/link.rs"));
        let ignored = HashSet::from(["src/visible.rs".to_owned()]);
        let result = WorkspaceIndex::default()
            .refresh(&root, &candidates, &ignored, truncated)
            .unwrap();
        assert_eq!(result.indexed_files, 0);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn repository_map_honors_budget_and_focus_order() {
        let root = fixture();
        for index in 0..40 {
            fs::write(
                root.join(format!("src/file_{index}.rs")),
                format!("pub fn symbol_{index}() {{}}\n"),
            )
            .unwrap();
        }
        let mut index = WorkspaceIndex::default();
        refresh(&root, &mut index);
        let map = index.repository_map(256, &["src/file_39.rs".into()]);
        assert!(map.estimated_tokens <= map.token_budget);
        assert!(map.truncated);
        assert_eq!(map.included_files.first().unwrap(), "src/file_39.rs");
        assert!(map.text.contains("function symbol_39"));
        fs::remove_dir_all(root).unwrap();
    }
}
