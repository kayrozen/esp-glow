#!/bin/bash
# HIL Test Runner Helper Script
# Usage: ./run_hil_tests.sh [OPTION]
#
# Enforces the task's run order (L0..L6, L8, then L7 soak last) by passing
# pytest an explicit file list -- pytest's default collection order is
# alphabetical, which would run L7 before L8.

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PORT="${GLOW_SERIAL_PORT:-/dev/ttyUSB0}"
DEVICE_IP="${GLOW_DEVICE_IP:-}"
VERBOSE="-v"
OUTPUT_FILE=""

# L0..L6, L8, then L7 (soak) last -- see pytest.ini's header comment.
ORDERED_LAYERS=(l0 l1 l2 l3 l4 l5 l6 l8 l7)

if ! command -v pytest &> /dev/null; then
    echo -e "${RED}ERROR: pytest not found${NC}"
    echo "Install dependencies: pip install -r requirements.txt"
    exit 1
fi
if ! python3 -c "import serial" 2>/dev/null; then
    echo -e "${RED}ERROR: pyserial not found${NC}"
    echo "Install dependencies: pip install -r requirements.txt"
    exit 1
fi

print_usage() {
    cat << EOF
ESP-Glow HIL Test Runner

Usage: $0 [OPTION]

Options:
  --all              Run every layer, L0-L6 + L8, then L7 soak last (15+ minutes)
  --soak             Run only L7 (10-minute soak)
  --quick            Run L0-L6 + L8, skip the L7 soak (default)
  --layer L0         Run one specific layer (L0-L8)
  --port PORT        Serial port (default: /dev/ttyUSB0, or \$GLOW_SERIAL_PORT)
  --device IP        Device IP address (required for L2/L4/L5/L6/L7; \$GLOW_DEVICE_IP)
  --skip-flash       Set GLOW_SKIP_FLASH=1 (assume the board already has a selftest build)
  --output FILE      Save an HTML report to FILE (needs pytest-html)
  -v, --verbose      Verbose output (default)
  -q, --quiet        Minimal output
  -h, --help         Show this help

Examples:
  $0 --device 192.168.1.42                 # Quick run (L0-L6, L8; no soak)
  $0 --all --device 192.168.1.42           # Full run including the soak
  $0 --soak --device 192.168.1.42          # Only the soak test
  $0 --layer L2 --device 192.168.1.42      # Only L2 (Art-Net)
  $0 --port /dev/ttyUSB1 --skip-flash      # Alternate port, skip build+flash

EOF
}

RUN_MODE="quick"
SKIP_FLASH=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --all) RUN_MODE="all"; shift ;;
        --soak) RUN_MODE="soak"; shift ;;
        --quick) RUN_MODE="quick"; shift ;;
        --layer) RUN_MODE="layer"; LAYER="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --device) DEVICE_IP="$2"; shift 2 ;;
        --skip-flash) SKIP_FLASH="1"; shift ;;
        --output) OUTPUT_FILE="$2"; shift 2 ;;
        -v|--verbose) VERBOSE="-vv"; shift ;;
        -q|--quiet) VERBOSE="-q"; shift ;;
        -h|--help) print_usage; exit 0 ;;
        *) echo -e "${RED}Unknown option: $1${NC}"; print_usage; exit 1 ;;
    esac
done

if [ ! -e "$PORT" ]; then
    echo -e "${YELLOW}WARNING: Serial port $PORT not found${NC}"
    echo "Available serial ports:"
    ls -la /dev/tty* 2>/dev/null || echo "  (none found)"
fi

export GLOW_SERIAL_PORT="$PORT"
if [ -n "$DEVICE_IP" ]; then
    export GLOW_DEVICE_IP="$DEVICE_IP"
fi
if [ -n "$SKIP_FLASH" ]; then
    export GLOW_SKIP_FLASH="$SKIP_FLASH"
fi

echo -e "${GREEN}=== ESP-Glow HIL Test Runner ===${NC}"
echo "Serial Port: $PORT"
echo "Device IP:  ${DEVICE_IP:-<not set>}"
echo "Mode:       $RUN_MODE"
echo ""

PYTEST_CMD=(pytest $VERBOSE --tb=short)
if [ -n "$OUTPUT_FILE" ]; then
    PYTEST_CMD+=(--html="$OUTPUT_FILE" --self-contained-html)
fi

files_for_layers() {
    for l in "$@"; do
        for f in test_${l}_*.py; do
            [ -e "$f" ] && echo "$f"
        done
    done
}

EXIT_CODE=0
case $RUN_MODE in
    quick)
        echo "Running L0-L6 and L8 (quick, no soak)..."
        mapfile -t FILES < <(files_for_layers l0 l1 l2 l3 l4 l5 l6 l8)
        "${PYTEST_CMD[@]}" "${FILES[@]}" || EXIT_CODE=$?
        ;;
    all)
        echo "Running the full suite in the specified order (L0-L6, L8, then L7 soak last)..."
        echo -e "${YELLOW}This will take 15+ minutes.${NC}"
        mapfile -t FILES < <(files_for_layers "${ORDERED_LAYERS[@]}")
        "${PYTEST_CMD[@]}" "${FILES[@]}" || EXIT_CODE=$?
        ;;
    soak)
        echo "Running L7 soak test only (10 minutes)..."
        "${PYTEST_CMD[@]}" test_l7_soak.py -s || EXIT_CODE=$?
        ;;
    layer)
        if [[ "$LAYER" =~ ^[Ll]([0-8])$ ]]; then
            LAYER_NUM="${BASH_REMATCH[1]}"
            echo "Running layer L${LAYER_NUM}..."
            "${PYTEST_CMD[@]}" test_l${LAYER_NUM}_*.py || EXIT_CODE=$?
        else
            echo -e "${RED}Invalid layer: $LAYER (must be L0-L8)${NC}"
            exit 1
        fi
        ;;
esac

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo -e "${GREEN}=== All tests passed ===${NC}"
else
    echo -e "${RED}=== Some tests failed ===${NC}"
    echo "Check the output above for details."
fi

if [ -n "$OUTPUT_FILE" ]; then
    echo "Test report: $OUTPUT_FILE"
fi

exit $EXIT_CODE
