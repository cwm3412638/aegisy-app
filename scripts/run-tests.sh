#!/bin/bash
# Test Runner Helper for Aegisy Coding Workbench
# Usage: ./scripts/run-tests.sh [category]

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: Build directory not found. Run 'cmake -B build' first."
    exit 1
fi

cd "$BUILD_DIR"

show_help() {
    cat << EOF
Test Runner Helper

Usage: ./scripts/run-tests.sh [category]

Categories:
    all             Run all tests (default)
    update          Update system tests (signing key ring, cache, artifact set)
    aap             AAP transport and protocol tests
    runtime         Agent runtime environment tests
    profile         Profile and tool management tests
    api             API client tests
    workspace       Workspace and workbench tests
    quick           Quick smoke tests only
    verbose         Run all tests with verbose output

Examples:
    ./scripts/run-tests.sh all
    ./scripts/run-tests.sh update
    ./scripts/run-tests.sh quick

EOF
}

run_tests() {
    local pattern="$1"
    local verbose="${2:-}"

    if [ "$verbose" = "verbose" ]; then
        ctest --output-on-failure -V -R "$pattern"
    else
        ctest --output-on-failure -R "$pattern"
    fi
}

case "${1:-all}" in
    all)
        echo "=== Running All Tests ==="
        ctest --output-on-failure
        ;;
    update)
        echo "=== Running Update System Tests ==="
        run_tests "UpdateSigningKeyRing|UpdateArtifactSet"
        ;;
    aap)
        echo "=== Running AAP Tests ==="
        run_tests "AapTransport|AapGenerated"
        ;;
    runtime)
        echo "=== Running Agent Runtime Tests ==="
        run_tests "AgentRuntime"
        ;;
    profile)
        echo "=== Running Profile Tests ==="
        run_tests "Profile|Tool|Skill"
        ;;
    api)
        echo "=== Running API Client Tests ==="
        run_tests "ApiClient"
        ;;
    workspace)
        echo "=== Running Workspace Tests ==="
        run_tests "Workspace|Workbench"
        ;;
    quick)
        echo "=== Running Quick Tests ==="
        run_tests "smoke|quick|basic"
        ;;
    verbose)
        echo "=== Running All Tests (Verbose) ==="
        ctest --output-on-failure -V
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        echo "Unknown category: $1"
        echo ""
        show_help
        exit 1
        ;;
esac

echo ""
echo "✓ Tests complete"
