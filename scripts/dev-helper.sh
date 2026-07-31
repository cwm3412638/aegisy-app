#!/bin/bash
# Aegisy Coding Workbench - Quick Development Helper
# Usage: ./scripts/dev-helper.sh [command]

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

show_help() {
    cat << EOF
Aegisy Coding Workbench - Development Helper

Usage: ./scripts/dev-helper.sh [command]

Commands:
    status          Show OpenSpec task progress
    build           Build the project (Release mode)
    build-debug     Build the project (Debug mode)
    test            Run all tests
    test-quick      Run quick tests only
    clean           Clean build artifacts
    docs            Generate/update documentation
    check-deps      Check system dependencies
    format          Format code (if formatters available)
    help            Show this help message

Examples:
    ./scripts/dev-helper.sh status
    ./scripts/dev-helper.sh build
    ./scripts/dev-helper.sh test

EOF
}

cmd_status() {
    echo "=== OpenSpec Status ==="
    if [ -f scripts/openspec-status.sh ]; then
        ./scripts/openspec-status.sh
    else
        echo "OpenSpec status script not found"
    fi
}

cmd_build() {
    echo "=== Building (Release) ==="
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    echo "✓ Build complete"
}

cmd_build_debug() {
    echo "=== Building (Debug) ==="
    cmake -B build -DCMAKE_BUILD_TYPE=Debug
    cmake --build build --parallel
    echo "✓ Debug build complete"
}

cmd_test() {
    echo "=== Running Tests ==="
    cd build && ctest --output-on-failure
    echo "✓ Tests complete"
}

cmd_test_quick() {
    echo "=== Running Quick Tests ==="
    cd build && ctest --output-on-failure -R "quick|smoke"
    echo "✓ Quick tests complete"
}

cmd_clean() {
    echo "=== Cleaning Build Artifacts ==="
    rm -rf build
    echo "✓ Clean complete"
}

cmd_docs() {
    echo "=== Documentation Status ==="
    echo "Main docs:"
    ls -1 docs/*.md 2>/dev/null | wc -l | xargs echo "  Markdown files:"
    echo ""
    echo "ADRs:"
    ls -1 docs/adr/*.md 2>/dev/null | wc -l | xargs echo "  ADR files:"
    echo ""
    echo "OpenSpec:"
    find openspec -name "*.md" 2>/dev/null | wc -l | xargs echo "  Spec files:"
}

cmd_check_deps() {
    echo "=== Checking Dependencies ==="

    check_cmd() {
        if command -v "$1" &> /dev/null; then
            echo "✓ $1: $(command -v $1)"
        else
            echo "✗ $1: not found"
        fi
    }

    check_cmd cmake
    check_cmd git
    check_cmd node
    check_cmd npm
    check_cmd cargo
    echo ""
    echo "Qt version:"
    qmake --version 2>/dev/null || echo "  qmake not in PATH"
}

cmd_format() {
    echo "=== Code Formatting ==="
    echo "C++ formatting not configured yet"
    echo "Rust formatting:"
    if [ -d agent-runtime ]; then
        cd agent-runtime && cargo fmt --check && echo "✓ Rust code is formatted"
    fi
}

# Main command dispatcher
case "${1:-help}" in
    status)
        cmd_status
        ;;
    build)
        cmd_build
        ;;
    build-debug)
        cmd_build_debug
        ;;
    test)
        cmd_test
        ;;
    test-quick)
        cmd_test_quick
        ;;
    clean)
        cmd_clean
        ;;
    docs)
        cmd_docs
        ;;
    check-deps)
        cmd_check_deps
        ;;
    format)
        cmd_format
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        echo "Unknown command: $1"
        echo ""
        show_help
        exit 1
        ;;
esac
