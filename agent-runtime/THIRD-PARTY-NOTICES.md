# Agent Runtime Third-Party Notices

The Aegisy Agent Runtime uses the following third-party components:

- lsp-types, version 0.97.0 (MIT), used for Language Server Protocol data types:
  https://github.com/gluon-lang/lsp-types
- sha2, version 0.10.9 (MIT OR Apache-2.0), used for content identities:
  https://github.com/RustCrypto/hashes
- similar, version 3.1.1 (Apache-2.0), used for bounded text diff previews:
  https://github.com/mitsuhiko/similar
- strip-ansi-escapes, version 0.2.1 (MIT OR Apache-2.0), used to normalize
  explicitly pinned recent terminal output before pinned-context hashing:
  https://github.com/luser/strip-ansi-escapes
- vte, version 0.14.1 (MIT OR Apache-2.0), used transitively by
  strip-ansi-escapes to parse terminal escape sequences:
  https://github.com/alacritty/vte
- base64, version 0.22.1 (MIT OR Apache-2.0), used to preserve PTY byte output
  across JSON-RPC:
  https://github.com/marshallpierce/rust-base64
- ed25519-dalek, version 2.1.1 (BSD-3-Clause), used to verify authenticated
  model-catalog signatures with strict Ed25519 validation:
  https://github.com/dalek-cryptography/ed25519-dalek
- image, version 0.25.10 (MIT OR Apache-2.0), used to validate and bound
  explicitly user-imported PNG, JPEG, and WebP pinned context and to create
  local UI thumbnails:
  https://github.com/image-rs/image
- libc, version 0.2.174 (MIT OR Apache-2.0), used for macOS process-group
  signal delivery:
  https://github.com/rust-lang/libc
- portable-pty, version 0.9.0 (MIT), used for the macOS PTY and Windows ConPTY
  backends:
  https://github.com/wezterm/wezterm/tree/main/pty
- windows-sys, version 0.61.2 (MIT OR Apache-2.0), used for Windows Job Object
  process-tree ownership:
  https://github.com/microsoft/windows-rs

- Tree-sitter Rust bindings, version 0.26.11:
  https://github.com/tree-sitter/tree-sitter
- Tree-sitter C++ grammar, version 0.23.4:
  https://github.com/tree-sitter/tree-sitter-cpp
- Tree-sitter JavaScript grammar, version 0.25.0:
  https://github.com/tree-sitter/tree-sitter-javascript
- Tree-sitter Python grammar, version 0.25.0:
  https://github.com/tree-sitter/tree-sitter-python
- Tree-sitter Rust grammar, version 0.24.2:
  https://github.com/tree-sitter/tree-sitter-rust
- Tree-sitter TypeScript and TSX grammars, version 0.23.2:
  https://github.com/tree-sitter/tree-sitter-typescript

Copyright notices and full license texts are retained in the corresponding Cargo
source packages and must be included in the generated release license inventory.
