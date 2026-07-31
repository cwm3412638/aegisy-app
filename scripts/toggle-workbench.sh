#!/bin/bash
# Toggle Agent Workbench feature flag

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

show_usage() {
    cat << EOF
Usage: $0 [enable|disable|status]

Toggle Agent Workbench feature flag.

Commands:
  enable   - Enable Agent Workbench
  disable  - Disable Agent Workbench
  status   - Show current status

Examples:
  $0 enable
  $0 disable
  $0 status
EOF
}

get_status() {
    if defaults read cc.aegisy.AegisyClient features/agentWorkbench 2>/dev/null | grep -q "1"; then
        echo "enabled"
    else
        echo "disabled"
    fi
}

enable_workbench() {
    defaults write cc.aegisy.AegisyClient features/agentWorkbench -bool true
    echo "✓ Agent Workbench enabled"
    echo "  Restart the application to see changes"
}

disable_workbench() {
    defaults delete cc.aegisy.AegisyClient features/agentWorkbench 2>/dev/null || true
    echo "✓ Agent Workbench disabled"
    echo "  Restart the application to see changes"
}

show_status() {
    STATUS=$(get_status)
    echo "Agent Workbench: $STATUS"
}

case "${1:-}" in
    enable)
        enable_workbench
        ;;
    disable)
        disable_workbench
        ;;
    status)
        show_status
        ;;
    *)
        show_usage
        exit 1
        ;;
esac
