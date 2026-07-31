#!/bin/bash
# OpenSpec Task Progress Tracker
# Usage: ./scripts/openspec-status.sh

set -euo pipefail

OPENSPEC_DIR="openspec/changes/build-aegisy-agent-workbench"
TASKS_FILE="$OPENSPEC_DIR/tasks.md"

echo "=== Aegisy Agent Workbench OpenSpec Status ==="
echo ""

# Count total tasks
TOTAL_TASKS=$(grep -c "^- \[" "$TASKS_FILE" || true)
COMPLETED_TASKS=$(grep -c "^- \[x\]" "$TASKS_FILE" || true)
PENDING_TASKS=$(grep -c "^- \[ \]" "$TASKS_FILE" || true)

echo "Total Tasks: $TOTAL_TASKS"
echo "Completed: $COMPLETED_TASKS"
echo "Pending: $PENDING_TASKS"
echo "Progress: $(awk "BEGIN {printf \"%.1f\", ($COMPLETED_TASKS/$TOTAL_TASKS)*100}")%"
echo ""

# Show section progress
echo "=== Section Progress ==="
echo ""

for section in {1..22}; do
    SECTION_NAME=$(grep "^## $section\." "$TASKS_FILE" | head -1 | sed 's/^## //')
    if [ -n "$SECTION_NAME" ]; then
        SECTION_TOTAL=$(sed -n "/^## $section\./,/^## $((section+1))\./p" "$TASKS_FILE" | grep -c "^- \[" || true)
        SECTION_DONE=$(sed -n "/^## $section\./,/^## $((section+1))\./p" "$TASKS_FILE" | grep -c "^- \[x\]" || true)

        if [ "$SECTION_TOTAL" -gt 0 ]; then
            SECTION_PCT=$(awk "BEGIN {printf \"%.0f\", ($SECTION_DONE/$SECTION_TOTAL)*100}")
            printf "%-60s %3d/%3d (%3d%%)\n" "$SECTION_NAME" "$SECTION_DONE" "$SECTION_TOTAL" "$SECTION_PCT"
        fi
    fi
done

echo ""
echo "=== Recent Progress ==="
echo ""

# Show recently modified task sections (tasks with notes added in last commit)
git log -1 --name-only --pretty="" | grep -q "$TASKS_FILE" && {
    echo "Tasks file updated in last commit"
    git diff HEAD~1 HEAD -- "$TASKS_FILE" | grep "^+" | grep -E "^\+  -" | head -5 || echo "No task updates found"
} || echo "No recent task file changes"

echo ""
