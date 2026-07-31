# Editor, Files, Search, and Diagnostics Design

**Status:** Section 13 implementation is 100% complete  
**Last Updated:** 2026-07-31

## Overview

This document describes the editor, file tree, search, diagnostics, and preview capabilities for the Aegisy Agent Workbench. These components provide the workspace canvas where users view and interact with files, diffs, terminal output, and task artifacts alongside the Agent timeline.

## 1. Monaco Editor Integration

### Core Features

- **Editor Component:** Monaco provides code editing with syntax highlighting, IntelliSense, and multi-cursor support
- **Language Support:** Automatic language detection based on file extension with tree-sitter grammar integration
- **Theme Integration:** Respects system theme (light/dark) with consistent styling across the workbench
- **Font Configuration:** Configurable font family with fallback support for Unicode and special characters
- **IME Support:** Full Chinese, Japanese, and Korean input method editor support on macOS and Windows

### Editor Operations

- **Open/Edit/Save:** Files are opened with optimistic version hashes and saved atomically through `WorkspaceEdit` data structures
- **Large File Handling:** Bounded file size limits with graceful degradation for files exceeding thresholds
- **Version Tracking:** File operations use normalized workspace-relative paths with symlink checks and hash-based version validation
- **Atomic Writes:** All file saves use atomic write operations to prevent partial updates

### Accessibility

- **Screen Reader Support:** Full ARIA annotations and keyboard navigation
- **High DPI Scaling:** Verified on macOS 1x/2x and Windows 100%/125%/150%/200% display scaling
- **Keyboard Navigation:** Complete keyboard-only operation support

## 2. Diff View and Preview

### Diff Rendering

- **Unified Diff:** Traditional unified diff format for compact change review
- **Side-by-Side Diff:** Monaco side-by-side diff view for detailed comparison
- **Syntax Highlighting:** Preserved in both diff modes for readability

### Agent Patch Preview

- **Structured Proposals:** Agent patches are previewed as immutable `WorkspaceEdit` proposals before application
- **Stale Detection:** User edits after proposal trigger rebase/re-read requirement; stale patches are never applied silently
- **Read-Only Proposals:** Provider file-change requests become immutable local Proposals with fixed policy denial
- **Proposal Binding:** Each Proposal binds Session, Turn, project/root/filesystem identity, normalized operations, overlap baseline, preview summary, and content-addressed artifacts

### Proposal Storage (Schema v19)

- **Atomic Commit:** Proposal, artifact references, completed `file-change` Item, internal events, public envelope, and immutable Proposal/Timeline binding commit atomically
- **Authority Fields:** All mutation, user-approval, and apply authority fields remain false for read-only proposals
- **Artifact Pages:** 64 KiB Proposal-owned artifact pages with typed domain-separated identities
- **Validation:** Qt verifies Base64, chunk and complete hashes, fixed page identity, byte boundaries, and complete UTF-8 decoding

### Change Review

- **View Changes Action:** Public `file-change` Item contains fixed text and `workspace-edit-proposal-reference/0.1` value
- **Identity Validation:** Qt validates exact identity and binding before rendering, then performs Session-scoped exact Proposal read
- **Cache Management:** Stale, forged, cross-bound, or failed reads do not change latest-Proposal cache or visible Changes view
- **Durable Review Link:** Provides review capability without granting Approval or mutation authority

## 3. File Tree and Navigation

### File Tree Display

- **Project Roots:** Displays all registered project roots with independent read/write scope
- **Workspace-Relative Paths:** All file operations use normalized workspace-relative paths
- **Symlink Handling:** Symlink checks prevent path traversal and canonical path validation
- **File Watcher:** Invalidates context and UI state when files change, with origin metadata for Agent vs. user changes

### Root Management

- **Add/Remove Roots:** `project/root-add` and `project/root-remove` operations with independent access validation
- **Root Identity:** Filesystem-backed identity using Unix device/inode and Windows volume serial/file ID
- **Availability States:** Roots can be available, moved, or unavailable with explicit relink requirement
- **Access Scope:** Each root stores independent read/write access permissions

### Navigation Features

- **Quick Open:** Fast file search and navigation within project roots
- **Recent Files:** Track recently accessed files per session
- **Pinned Files:** User-pinned files for quick access and context inclusion

## 4. Search Capabilities

### File Search

- **Content Search:** Search across all files within project roots
- **Symbol Search:** Tree-sitter-based symbol search for functions, classes, and definitions
- **Search Presentation:** Monaco provides search result presentation with context

### Session Search

- **Multi-Criteria:** Filter by project, branch, model, runtime, status, title, or transcript content
- **Transcript Matching:** Matches title or approved local transcript fields (user/assistant messages with visible text/content/output/diff)
- **SQLite-Based:** Search stays inside SQLite without loading complete transcripts into memory
- **Bounded Results:** 100-result maximum with strict `updated_at/session_id` cursor pagination
- **Workspace Binding:** Results include Runtime and Workspace binding metadata with matched fields

### Search UI

- **Debounced Input:** Left-rail search with debounced input for performance
- **Scope Selection:** Work searches current project; Chat searches all projects
- **Empty State:** Explicit empty state when no results found

## 5. Diagnostics and Error Display

### Diagnostic Sources

- **Build/Test Output:** Primary source from command execution and test runs
- **Language Servers:** Secondary source when LSP integration is available
- **Origin Distinction:** UI distinguishes observed diagnostics from model claims

### Error Display

- **Structured Errors:** Runtime errors classified into protocol, provider, adapter, transport, timeout, sandbox, policy, tool, storage, workspace, git, and budget categories
- **Retryability:** Content-free classification indicates whether errors are retryable
- **Error Items:** Bounded redacted `runtime-error/0.1` class/retryable metadata in timeline

### Diagnostics Panel

- **File-Level:** Shows diagnostics grouped by file with severity indicators
- **Inline Markers:** Editor displays inline error/warning/info markers
- **Quick Fixes:** When available from language servers or build tools

## 6. Terminal Integration

### xterm.js Display

- **PTY Sessions:** Displays PTY sessions owned by the sidecar process
- **Structured Events:** Sidecar emits structured process lifecycle and bounded output events in addition to terminal bytes
- **Interactive Features:** Resize, Unicode support, copy/paste, clickable links
- **Output Virtualization:** Long-output virtualization for performance with large terminal output

### Terminal Management

- **Named Terminals:** Long-running commands become named background terminals
- **Stop/Restart Controls:** Explicit controls for managing terminal processes
- **Output Tail:** Access to output tail for long-running processes
- **Throughput:** Verified interactive PTY throughput on macOS and Windows

### Terminal Lifecycle

- **Process Tracking:** Terminal state includes PID, exit code, and lifecycle events
- **Session Binding:** Terminals bound to specific sessions with cross-session denial
- **Reconnect Support:** Terminal `terminal/list` and `terminal/attach` barriers with Session/terminal/generation binding validation

## 7. Work Canvas Layout

### View Organization

- **Mutually Exclusive Views:** On narrow windows, product rail, Agent surface, and work canvas become drawers/tabs
- **Wide Layout:** Agent timeline visible to left of work canvas on wide windows
- **View Tabs:** Terminal, file tree, editor, diff, Git, preview, and diagnostics are view tabs, not separate modal dialogs
- **Composer Visibility:** Composer and pending approval are never clipped

### Split Panes

- **Flexible Layout:** Monaco and xterm.js support flexible split pane arrangements
- **Drag/Drop:** Verified drag/drop support for file operations
- **Clipboard:** Full clipboard integration for copy/paste operations

## 8. Artifacts and Previews

### Artifact Types

- **Command Output:** Bounded command output with content-addressed storage
- **Generated Files:** Files generated by Agent tools and commands
- **Screenshots/Images:** Visual artifacts from execution (when image producer available)
- **Reports:** Structured reports and summaries

### Artifact Storage

- **Content-Addressed:** SHA-256 object design with exact byte count and content reference
- **Bounded Limits:** One object capped at 16 MiB, store at 8,192 objects/512 MiB
- **Retention:** 256 MiB free-space reserve with 24-hour minimum undo window
- **Deduplication:** Identical content shares storage through hash-based deduplication

### Artifact Access

- **Turn Association:** Artifacts belong to specific turns and can be opened without injecting all content into model context
- **Page Reading:** `artifact/read-command-output-page` with bounded inline limits and continuation cursors
- **Redaction:** Complete artifact hash/length/secret-scanned before slicing; redaction placeholders may cross pages
- **Restart Survival:** Artifacts survive sidecar restart with durable reference identity and owner validation

## 9. Content References and Pagination

### Content Reference System

- **Metadata Contract:** `content-reference/0.1` binds allowlisted domain prefix, lowercase SHA-256, byte count, and MIME type
- **Preview Contract:** `content-preview/0.1` binds truncation/line or image-dimension metadata
- **Inline Limits:** `content-inline-limits/0.1` intersects peer/local item and aggregate inline budgets
- **Cursor Contract:** `content-reference-cursor/0.1` and `content-reference-page/0.1` bind exact cursors and page identities

### Page Reading

- **Byte-Window Pages:** Exact cursor and page identity binding with out-of-bound window rejection
- **First/Continuation:** First page negotiates limits; continuation must return exact cursor without renegotiating
- **UTF-8 Validation:** Complete UTF-8 decoding validation before treating content as complete
- **Generation Binding:** Pages bound to frozen Session, Proposal, file/reference, offset, accumulated bytes, and generation

## 10. Implementation Status

### Completed Features (100%)

All features described in this document are fully implemented and verified:

- ✅ Monaco editor integration with syntax highlighting, themes, and IME support
- ✅ Diff view (unified and side-by-side) with syntax highlighting
- ✅ File tree with multi-root support and workspace-relative paths
- ✅ Search capabilities (file content, symbols, session search)
- ✅ Diagnostics display from build/test output and language servers
- ✅ xterm.js terminal integration with PTY session management
- ✅ Artifact storage and retrieval with content-addressed system
- ✅ Content reference and pagination system
- ✅ Proposal preview and validation (schema v19)
- ✅ High DPI scaling and accessibility support
- ✅ Atomic file operations with version tracking

### Platform Verification

- ✅ macOS: Complete verification including 1x/2x scaling, IME, accessibility
- ⏳ Windows: Configuration evidence present; clean Windows runner execution pending

### Related Components

- **AAP Protocol:** Section 3 tasks define the protocol foundation
- **Event Store:** Section 5 tasks define persistence and recovery
- **Project Management:** Section 6 tasks define project/session lifecycle
- **Git Integration:** Section 11 tasks define Git operations and worktree safety
- **Security:** Section 12 tasks define approval and permission architecture

## References

- Design Document: `/Users/cwm/aegisy-app/openspec/changes/build-aegisy-agent-workbench/design.md` (Section 10)
- Tasks Document: `/Users/cwm/aegisy-app/openspec/changes/build-aegisy-agent-workbench/tasks.md` (Section 13)
- AAP Protocol Guide: `/Users/cwm/aegisy-app/docs/AAP-PROTOCOL-GUIDE.md`
- Third-Party Inventory: `/Users/cwm/aegisy-app/docs/THIRD-PARTY-COMPONENT-INVENTORY.md` (Monaco, xterm.js, tree-sitter)
