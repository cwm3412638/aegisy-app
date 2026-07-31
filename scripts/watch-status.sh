#!/bin/bash
# Real-time development status monitor
# Usage: ./scripts/watch-status.sh

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

clear

while true; do
    tput cup 0 0

    echo "╔════════════════════════════════════════════════════════════════╗"
    echo "║        Aegisy Coding Workbench - Development Status           ║"
    echo "╚════════════════════════════════════════════════════════════════╝"
    echo ""

    # Git status
    echo "📊 Git Status:"
    echo "  Branch: $(git branch --show-current)"
    echo "  Commits ahead: $(git rev-list --count @{u}..HEAD 2>/dev/null || echo '0')"
    echo "  Modified files: $(git status --short | wc -l | tr -d ' ')"
    echo ""

    # Build status
    echo "🔨 Build Status:"
    if [ -d "build" ]; then
        echo "  Build directory: ✓ exists"
        if [ -f "build/Aegisy" ] || [ -f "build/Aegisy.exe" ]; then
            echo "  Executable: ✓ built"
        else
            echo "  Executable: ✗ not found"
        fi
    else
        echo "  Build directory: ✗ missing"
    fi
    echo ""

    # OpenSpec progress
    echo "📋 OpenSpec Progress:"
    if [ -f "openspec/changes/build-aegisy-agent-workbench/tasks.md" ]; then
        TOTAL=$(grep -c "^- \[" openspec/changes/build-aegisy-agent-workbench/tasks.md || echo "0")
        DONE=$(grep -c "^- \[x\]" openspec/changes/build-aegisy-agent-workbench/tasks.md || echo "0")
        PENDING=$(grep -c "^- \[ \]" openspec/changes/build-aegisy-agent-workbench/tasks.md || echo "0")
        PCT=$((DONE * 100 / TOTAL))
        echo "  Total tasks: $TOTAL"
        echo "  Completed: $DONE ($PCT%)"
        echo "  Pending: $PENDING"
    fi
    echo ""

    # Recent activity
    echo "🕐 Recent Activity:"
    git log --oneline -3 | sed 's/^/  /'
    echo ""

    # Documentation count
    echo "📚 Documentation:"
    echo "  Markdown files: $(find docs -name "*.md" 2>/dev/null | wc -l | tr -d ' ')"
    echo "  ADRs: $(find docs/adr -name "*.md" 2>/dev/null | wc -l | tr -d ' ')"
    echo ""

    # Process status
    echo "⚙️  Processes:"
    AEGISY_PROCS=$(ps aux | grep -i aegisy | grep -v grep | wc -l | tr -d ' ')
    echo "  Aegisy processes: $AEGISY_PROCS"
    echo ""

    echo "Press Ctrl+C to exit | Refreshing every 5 seconds..."

    sleep 5
done
