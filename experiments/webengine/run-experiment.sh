#!/bin/bash
# WebEngine Experiments Launcher
# Usage: ./run-experiment.sh [experiment-name]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../../build"

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${BLUE}=== Aegisy WebEngine Experiments ===${NC}\n"

# Check if build exists
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}Build directory not found. Building...${NC}"
    cmake -B "$BUILD_DIR" -DAEGISY_BUILD_WEBENGINE_EXPERIMENT=ON
    cmake --build "$BUILD_DIR" -j8
fi

# Available experiments
declare -A experiments=(
    ["1"]="aegisy_webengine_experiment|Basic WebEngine (Task 2.1)"
    ["2"]="aegisy_webengine_secure_bundle|Secure Bundle (Task 2.2)"
    ["3"]="aegisy_webengine_webchannel_bridge|QWebChannel Bridge (Task 2.3)"
    ["4"]="aegisy_webengine_monaco_editor|Monaco Editor (Task 2.4)"
    ["5"]="aegisy_webengine_xterm_terminal|xterm.js Terminal (Task 2.5)"
)

# If argument provided, run directly
if [ -n "$1" ]; then
    case "$1" in
        basic|1) exec "$BUILD_DIR/aegisy_webengine_experiment" ;;
        secure|2) exec "$BUILD_DIR/aegisy_webengine_secure_bundle" ;;
        bridge|3) exec "$BUILD_DIR/aegisy_webengine_webchannel_bridge" ;;
        monaco|4) exec "$BUILD_DIR/aegisy_webengine_monaco_editor" ;;
        xterm|5) exec "$BUILD_DIR/aegisy_webengine_xterm_terminal" ;;
        *) echo "Unknown experiment: $1"; exit 1 ;;
    esac
fi

# Interactive menu
echo "Select experiment to run:"
for key in $(echo "${!experiments[@]}" | tr ' ' '\n' | sort); do
    IFS='|' read -r target desc <<< "${experiments[$key]}"
    echo -e "  ${GREEN}$key${NC}) $desc"
done
echo ""

read -p "Enter choice (1-5): " choice

if [ -z "$choice" ]; then
    echo "No selection made"
    exit 0
fi

if [ -n "${experiments[$choice]}" ]; then
    IFS='|' read -r target desc <<< "${experiments[$choice]}"
    echo -e "\n${BLUE}Launching: $desc${NC}\n"
    exec "$BUILD_DIR/$target"
else
    echo "Invalid choice: $choice"
    exit 1
fi
