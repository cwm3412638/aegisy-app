#!/bin/bash
# Commit statistics and analysis
# Usage: ./scripts/commit-stats.sh [days]

set -euo pipefail

DAYS="${1:-30}"

echo "=== Commit Statistics (Last $DAYS Days) ==="
echo ""

# Total commits
TOTAL=$(git log --since="$DAYS days ago" --oneline | wc -l | tr -d ' ')
echo "Total commits: $TOTAL"
echo ""

# Commits by author
echo "Commits by author:"
git log --since="$DAYS days ago" --format='%an' | sort | uniq -c | sort -rn | sed 's/^/  /'
echo ""

# Commits by type
echo "Commits by type:"
git log --since="$DAYS days ago" --format='%s' | grep -oE '^[a-z]+' | sort | uniq -c | sort -rn | sed 's/^/  /'
echo ""

# Files changed
echo "Most changed files:"
git log --since="$DAYS days ago" --name-only --format='' | sort | uniq -c | sort -rn | head -10 | sed 's/^/  /'
echo ""

# Lines added/removed
echo "Lines changed:"
git log --since="$DAYS days ago" --numstat --format='' | awk '{add+=$1; del+=$2} END {print "  Added: " add "\n  Removed: " del "\n  Net: " add-del}'
echo ""

# Activity by day
echo "Activity by day of week:"
git log --since="$DAYS days ago" --format='%ad' --date=format:'%A' | sort | uniq -c | sort -rn | sed 's/^/  /'
echo ""

# Recent milestones
echo "Recent milestones (tags):"
git tag --sort=-creatordate | head -5 | sed 's/^/  /'
