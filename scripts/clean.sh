#!/bin/bash
# Clean build artifacts and caches
# Usage: ./scripts/clean.sh [--deep]

set -euo pipefail

DEEP_CLEAN="${1:-}"

echo "=== Cleaning Build Artifacts ==="
echo ""

# Build directory
if [ -d "build" ]; then
    echo "Removing build directory..."
    rm -rf build
    echo "✓ Build directory removed"
else
    echo "✓ Build directory already clean"
fi

# CMake cache
if [ -f "CMakeCache.txt" ]; then
    echo "Removing CMake cache..."
    rm -f CMakeCache.txt
    echo "✓ CMake cache removed"
fi

# Qt generated files
echo "Cleaning Qt generated files..."
find . -name "*.autogen" -type d -exec rm -rf {} + 2>/dev/null || true
find . -name "moc_*.cpp" -delete 2>/dev/null || true
find . -name "ui_*.h" -delete 2>/dev/null || true
find . -name "qrc_*.cpp" -delete 2>/dev/null || true
echo "✓ Qt generated files cleaned"

# Rust build artifacts
if [ -d "agent-runtime/target" ]; then
    echo "Cleaning Rust build artifacts..."
    cd agent-runtime
    cargo clean
    cd ..
    echo "✓ Rust artifacts cleaned"
fi

if [ "$DEEP_CLEAN" = "--deep" ]; then
    echo ""
    echo "=== Deep Clean ==="
    echo ""

    # ccache
    if command -v ccache &> /dev/null; then
        echo "Clearing ccache..."
        ccache -C
        echo "✓ ccache cleared"
    fi

    # sccache
    if command -v sccache &> /dev/null; then
        echo "Clearing sccache..."
        sccache --stop-server 2>/dev/null || true
        echo "✓ sccache cleared"
    fi

    # Node modules (if any)
    if [ -d "node_modules" ]; then
        echo "Removing node_modules..."
        rm -rf node_modules
        echo "✓ node_modules removed"
    fi

    # Package locks
    if [ -f "package-lock.json" ]; then
        echo "Removing package-lock.json..."
        rm -f package-lock.json
        echo "✓ package-lock.json removed"
    fi

    # Temporary files
    echo "Removing temporary files..."
    find . -name "*.tmp" -delete 2>/dev/null || true
    find . -name "*.bak" -delete 2>/dev/null || true
    find . -name "*~" -delete 2>/dev/null || true
    find . -name ".DS_Store" -delete 2>/dev/null || true
    echo "✓ Temporary files removed"

    # IDE files
    echo "Removing IDE files..."
    rm -rf .vscode/.cache 2>/dev/null || true
    rm -rf .idea 2>/dev/null || true
    echo "✓ IDE files cleaned"
fi

echo ""
echo "=== Clean Complete ==="
echo ""
echo "To rebuild:"
echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build build --parallel"
