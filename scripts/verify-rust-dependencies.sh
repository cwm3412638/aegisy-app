#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$ROOT_DIR/agent-runtime/Cargo.toml"
EXPECTED_DENY_VERSION="0.19.9"
CARGO_BIN="${CARGO_BIN:-${HOME}/.cargo/bin/cargo}"
CARGO_DENY_BIN="${CARGO_DENY_BIN:-${HOME}/.cargo/bin/cargo-deny}"

if [[ ! -x "$CARGO_BIN" ]]; then
    CARGO_BIN="$(command -v cargo || true)"
fi
if [[ -z "$CARGO_BIN" || ! -x "$CARGO_BIN" ]]; then
    echo "cargo is required for the locked dependency audit" >&2
    exit 1
fi
export PATH="$(dirname "$CARGO_BIN"):$PATH"
if [[ ! -x "$CARGO_DENY_BIN" ]]; then
    CARGO_DENY_BIN="$(command -v cargo-deny || true)"
fi
if [[ -z "$CARGO_DENY_BIN" || ! -x "$CARGO_DENY_BIN" ]]; then
    echo "cargo-deny ${EXPECTED_DENY_VERSION} is required; refusing to skip dependency audit" >&2
    exit 1
fi

installed_version="$($CARGO_DENY_BIN --version)"
if [[ "$installed_version" != "cargo-deny ${EXPECTED_DENY_VERSION}"* ]]; then
    echo "expected cargo-deny ${EXPECTED_DENY_VERSION}, got: ${installed_version}" >&2
    exit 1
fi

cd "$ROOT_DIR"
metadata_file="$(mktemp "${TMPDIR:-/tmp}/aegisy-cargo-metadata.XXXXXX")"
trap 'rm -f "$metadata_file"' EXIT
"$CARGO_BIN" metadata --locked --manifest-path "$MANIFEST" --format-version 1 > "$metadata_file"
(cd "$ROOT_DIR/agent-runtime" && "$CARGO_DENY_BIN" check \
    --metadata-path "$metadata_file" --config "$ROOT_DIR/deny.toml")
