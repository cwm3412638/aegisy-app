#!/bin/bash
# Build and verify all WebEngine experiments
# Usage: ./test-experiments.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${SCRIPT_DIR}/../.."
BUILD_DIR="${PROJECT_ROOT}/build"

GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}=== Testing WebEngine Experiments ===${NC}\n"

# Clean and configure
echo "Configuring CMake..."
if ! cmake -B "$BUILD_DIR" -DAEGISY_BUILD_WEBENGINE_EXPERIMENT=ON > /dev/null 2>&1; then
    echo -e "${RED}CMake configuration failed${NC}"
    cmake -B "$BUILD_DIR" -DAEGISY_BUILD_WEBENGINE_EXPERIMENT=ON
    exit 1
fi

# Build all targets
targets=(
    "aegisy_webengine_experiment"
    "aegisy_webengine_secure_bundle"
    "aegisy_webengine_webchannel_bridge"
    "aegisy_webengine_monaco_editor"
    "aegisy_webengine_xterm_terminal"
)

echo -e "\nBuilding targets..."
for target in "${targets[@]}"; do
    echo -n "  Building $target... "
    if cmake --build "$BUILD_DIR" --target "$target" > /dev/null 2>&1; then
        echo -e "${GREEN}✓${NC}"
    else
        echo -e "${RED}✗${NC}"
        exit 1
    fi
done

# Verify executables exist
echo -e "\nVerifying executables..."
for target in "${targets[@]}"; do
    exe="$BUILD_DIR/$target"
    echo -n "  Checking $target... "
    if [ -f "$exe" ] && [ -x "$exe" ]; then
        echo -e "${GREEN}✓${NC}"
    else
        echo -e "${RED}✗ (not found or not executable)${NC}"
        exit 1
    fi
done

echo -e "\n${GREEN}All experiments built successfully!${NC}"
echo -e "\nRun experiments with: ${BLUE}./run-experiment.sh${NC}"
