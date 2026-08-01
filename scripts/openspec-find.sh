#!/bin/bash
# Find and filter OpenSpec tasks

set -euo pipefail

TASKS_FILE="openspec/changes/build-aegisy-agent-workbench/tasks.md"

usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Find and filter OpenSpec tasks by various criteria.

OPTIONS:
    -s, --section N     Show tasks in section N
    -i, --incomplete    Show only incomplete tasks
    -c, --complete      Show only completed tasks
    -k, --keyword TEXT  Search for keyword in task description
    -n, --next          Show next actionable tasks (incomplete, no Windows)
    -h, --help          Show this help message

EXAMPLES:
    $0 --section 10              # Show all Section 10 tasks
    $0 --incomplete --section 23 # Show incomplete Section 23 tasks
    $0 --keyword "test"          # Find tasks mentioning "test"
    $0 --next                    # Show next actionable tasks

EOF
}

show_section() {
    local section=$1
    echo "=== Section $section Tasks ==="
    awk "/^## $section\./,/^## [0-9]+\./" "$TASKS_FILE" | grep -E "^- \[" | head -20
}

show_incomplete() {
    local section=${1:-}
    if [ -n "$section" ]; then
        awk "/^## $section\./,/^## [0-9]+\./" "$TASKS_FILE" | grep "^- \[ \]"
    else
        grep "^- \[ \]" "$TASKS_FILE"
    fi
}

show_complete() {
    local section=${1:-}
    if [ -n "$section" ]; then
        awk "/^## $section\./,/^## [0-9]+\./" "$TASKS_FILE" | grep "^- \[x\]"
    else
        grep "^- \[x\]" "$TASKS_FILE"
    fi
}

search_keyword() {
    local keyword=$1
    echo "=== Tasks matching '$keyword' ==="
    grep -B 1 -A 3 -i "$keyword" "$TASKS_FILE" | grep -E "^- \[" | head -20
}

show_next_actionable() {
    echo "=== Next Actionable Tasks ==="
    echo ""
    echo "Incomplete tasks (excluding Windows/approval):"
    echo ""

    # Find incomplete tasks
    grep "^- \[ \]" "$TASKS_FILE" | head -20
}

# Parse arguments
SECTION=""
MODE=""
KEYWORD=""

while [[ $# -gt 0 ]]; do
    case $1 in
        -s|--section)
            SECTION="$2"
            shift 2
            ;;
        -i|--incomplete)
            MODE="incomplete"
            shift
            ;;
        -c|--complete)
            MODE="complete"
            shift
            ;;
        -k|--keyword)
            KEYWORD="$2"
            shift 2
            ;;
        -n|--next)
            MODE="next"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Execute based on mode
if [ -n "$KEYWORD" ]; then
    search_keyword "$KEYWORD"
elif [ "$MODE" = "next" ]; then
    show_next_actionable
elif [ "$MODE" = "incomplete" ]; then
    show_incomplete "$SECTION"
elif [ "$MODE" = "complete" ]; then
    show_complete "$SECTION"
elif [ -n "$SECTION" ]; then
    show_section "$SECTION"
else
    usage
fi
