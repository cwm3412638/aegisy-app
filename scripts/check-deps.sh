#!/bin/bash
# Dependency checker and updater
# Usage: ./scripts/check-deps.sh [--update]

set -euo pipefail

UPDATE_MODE="${1:-}"

echo "=== Dependency Check ==="
echo ""

check_version() {
    local name=$1
    local cmd=$2
    local version_arg=${3:---version}

    if command -v "$cmd" &> /dev/null; then
        version=$($cmd $version_arg 2>&1 | head -1)
        echo "✓ $name: $version"
        return 0
    else
        echo "✗ $name: not found"
        return 1
    fi
}

# System tools
echo "System Tools:"
check_version "CMake" "cmake" "--version"
check_version "Git" "git" "--version"
check_version "Make" "make" "--version"
check_version "Ninja" "ninja" "--version" || true
echo ""

# Compilers
echo "Compilers:"
check_version "GCC" "gcc" "--version" || true
check_version "Clang" "clang" "--version" || true
check_version "MSVC" "cl" "" || true
echo ""

# Qt
echo "Qt:"
check_version "qmake" "qmake" "--version"
check_version "Qt6" "qmake" "-query QT_VERSION" || check_version "Qt5" "qmake" "-query QT_VERSION"
echo ""

# Rust
echo "Rust:"
check_version "rustc" "rustc" "--version"
check_version "cargo" "cargo" "--version"
check_version "cargo-deny" "cargo-deny" "--version" || echo "  (optional, install with: cargo install cargo-deny)"
echo ""

# Node.js
echo "Node.js:"
check_version "node" "node" "--version"
check_version "npm" "npm" "--version"
echo ""

# Build tools
echo "Build Tools:"
check_version "ccache" "ccache" "--version" || echo "  (optional, speeds up builds)"
check_version "sccache" "sccache" "--version" || echo "  (optional, Rust build cache)"
echo ""

# Platform-specific
if [[ "$OSTYPE" == "darwin"* ]]; then
    echo "macOS Tools:"
    check_version "Homebrew" "brew" "--version"
    check_version "codesign" "codesign" "--version"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    echo "Linux Tools:"
    check_version "apt" "apt" "--version" || check_version "yum" "yum" "--version" || true
elif [[ "$OSTYPE" == "msys" ]] || [[ "$OSTYPE" == "win32" ]]; then
    echo "Windows Tools:"
    check_version "choco" "choco" "--version" || true
fi
echo ""

# Rust dependencies
if [ -f "agent-runtime/Cargo.lock" ]; then
    echo "Rust Dependencies:"
    cd agent-runtime
    if [ "$UPDATE_MODE" = "--update" ]; then
        echo "Updating Rust dependencies..."
        cargo update
    else
        echo "Run with --update to update Rust dependencies"
    fi
    cd ..
    echo ""
fi

# Summary
echo "=== Summary ==="
echo "All required dependencies are installed."
echo "Optional dependencies can improve build performance."
echo ""
echo "To install missing dependencies:"
echo "  macOS: brew install <package>"
echo "  Linux: sudo apt install <package>"
echo "  Windows: choco install <package>"
