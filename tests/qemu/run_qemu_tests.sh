#!/bin/bash
# QEMU Boot Test Runner Helper Script
# Usage: ./run_qemu_tests.sh [OPTION]

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

QEMU_BIN="${GLOW_QEMU_BIN:-qemu-system-xtensa}"
SKIP_BUILD=""
VERBOSE="-v"

print_usage() {
    cat << EOF
ESP-Glow QEMU Boot Test Runner

Usage: $0 [OPTION]

Options:
  --skip-build       Set GLOW_SKIP_BUILD=1 (assume GLOW_IDF_BUILD_DIR already
                      holds a selftest build; useful for iterating on the
                      test suite itself without a multi-minute rebuild)
  --qemu-bin PATH     Path to qemu-system-xtensa (default: qemu-system-xtensa
                      on PATH, or \$GLOW_QEMU_BIN)
  -v, --verbose       Verbose output (default)
  -q, --quiet         Minimal output
  -h, --help          Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --skip-build) SKIP_BUILD="1"; shift ;;
        --qemu-bin) QEMU_BIN="$2"; shift 2 ;;
        -v|--verbose) VERBOSE="-vv"; shift ;;
        -q|--quiet) VERBOSE="-q"; shift ;;
        -h|--help) print_usage; exit 0 ;;
        *) echo -e "${RED}Unknown option: $1${NC}"; print_usage; exit 1 ;;
    esac
done

if ! command -v pytest &> /dev/null; then
    echo -e "${RED}ERROR: pytest not found${NC}"
    echo "Install dependencies: pip install -r requirements.txt"
    exit 1
fi
if ! command -v "$QEMU_BIN" &> /dev/null; then
    echo -e "${RED}ERROR: $QEMU_BIN not found on PATH${NC}"
    echo "See README.md's Prerequisites section for how to get one."
    exit 1
fi
if [ -z "$SKIP_BUILD" ] && ! command -v idf.py &> /dev/null; then
    echo -e "${RED}ERROR: idf.py not found on PATH${NC}"
    echo "Source an ESP-IDF environment (. \$IDF_PATH/export.sh), or pass --skip-build."
    exit 1
fi

export GLOW_QEMU_BIN="$QEMU_BIN"
if [ -n "$SKIP_BUILD" ]; then
    export GLOW_SKIP_BUILD="$SKIP_BUILD"
fi

echo -e "${GREEN}=== ESP-Glow QEMU Boot Test Runner ===${NC}"
echo "QEMU binary: $QEMU_BIN"
echo "Skip build:  ${SKIP_BUILD:-no}"
echo ""

EXIT_CODE=0
pytest $VERBOSE --tb=short || EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo -e "${GREEN}=== All tests passed ===${NC}"
else
    echo -e "${RED}=== Some tests failed ===${NC}"
fi

exit $EXIT_CODE
