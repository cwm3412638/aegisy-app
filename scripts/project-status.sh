#!/bin/bash
# Generate comprehensive project status report

set -euo pipefail

echo "=== Aegisy Agent Workbench - Project Status ==="
echo ""
echo "Generated: $(date '+%Y-%m-%d %H:%M:%S')"
echo ""

# Git status
echo "=== Git Status ==="
git status --short --branch
echo ""

# Recent commits
echo "=== Recent Commits (Last 5) ==="
git log --oneline -5
echo ""

# OpenSpec progress
echo "=== OpenSpec Progress ==="
if [ -f scripts/openspec-status.sh ]; then
    ./scripts/openspec-status.sh 2>/dev/null | head -40
else
    echo "OpenSpec status script not found"
fi
echo ""

# Documentation
echo "=== Documentation Files ==="
find docs -name "*.md" -type f | wc -l | xargs echo "Total markdown files:"
echo ""
echo "Key documentation:"
ls -lh docs/*.md 2>/dev/null | awk '{print $9, $5}' | head -10
echo ""

# Code statistics
echo "=== Code Statistics ==="
echo "C++ files:"
find src include -name "*.cpp" -o -name "*.h" | wc -l | xargs echo "  Total:"
find src -name "*.cpp" | xargs wc -l 2>/dev/null | tail -1 | awk '{print "  Lines:", $1}'
echo ""
echo "Rust files:"
find agent-runtime -name "*.rs" | wc -l | xargs echo "  Total:"
find agent-runtime -name "*.rs" | xargs wc -l 2>/dev/null | tail -1 | awk '{print "  Lines:", $1}'
echo ""

# Tests
echo "=== Tests ==="
find tests -name "*.cpp" | wc -l | xargs echo "Test files:"
if [ -d build ]; then
    echo "Build directory exists"
    if command -v ctest &> /dev/null; then
        echo "CTest available"
    fi
else
    echo "No build directory"
fi
echo ""

# Dependencies
echo "=== Dependencies ==="
if command -v cmake &> /dev/null; then
    cmake --version | head -1
fi
if command -v rustc &> /dev/null; then
    rustc --version
fi
if command -v cargo &> /dev/null; then
    cargo --version
fi
echo ""

# Build status
echo "=== Build Status ==="
if [ -d build ]; then
    if [ -f build/AegisyClient ] || [ -f build/AegisyClient.app/Contents/MacOS/AegisyClient ]; then
        echo "✓ Desktop client built"
    else
        echo "✗ Desktop client not built"
    fi
    if [ -f build/agent-runtime/target/release/aegisy-agentd ] || [ -f build/agent-runtime/target/debug/aegisy-agentd ]; then
        echo "✓ Runtime sidecar built"
    else
        echo "✗ Runtime sidecar not built"
    fi
else
    echo "No build directory"
fi
echo ""

# Project size
echo "=== Project Size ==="
du -sh . 2>/dev/null | awk '{print "Total:", $1}'
du -sh build 2>/dev/null | awk '{print "Build:", $1}' || echo "Build: N/A"
du -sh agent-runtime/target 2>/dev/null | awk '{print "Rust target:", $1}' || echo "Rust target: N/A"
echo ""

echo "=== Summary ==="
echo "Project: Aegisy Agent Workbench"
echo "Status: Active Development (42% complete)"
echo "Platform: macOS (primary), Windows (testing needed)"
echo "Architecture: Multi-process (Qt + Rust)"
echo ""
echo "For detailed progress, see:"
echo "  - openspec/changes/build-aegisy-agent-workbench/tasks.md"
echo "  - docs/progress/"
echo "  - ARCHITECTURE.md"
echo "  - README.md"
