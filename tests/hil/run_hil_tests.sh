#!/bin/bash
# HIL Test Runner Helper Script
# Usage: ./run_hil_tests.sh [OPTION]

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Defaults
PORT="${GLOW_SERIAL_PORT:-/dev/ttyUSB0}"
DEVICE_IP="${GLOW_DEVICE_IP:-192.168.1.100}"
VERBOSE="-v"
RUN_SOAK=false
OUTPUT_FILE=""

# Check if pytest is installed
if ! command -v pytest &> /dev/null; then
    echo -e "${RED}ERROR: pytest not found${NC}"
    echo "Install dependencies: pip install -r requirements.txt"
    exit 1
fi

# Check if pyserial is installed
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
  --all              Run all tests including 10-minute soak (default: quick tests only)
  --soak             Run only L5 soak test (10 minutes)
  --quick            Run L0-L4, L6 only (skip soak) - default
  --layer L0         Run specific layer (L0-L6)
  --port PORT        Serial port (default: /dev/ttyUSB0)
  --device IP        Device IP address (default: 192.168.1.100)
  --output FILE      Save test output to FILE
  -v, --verbose      Verbose output (default)
  -q, --quiet        Minimal output
  -h, --help         Show this help

Examples:
  $0                        # Quick run (L0-L6, no soak)
  $0 --all                  # Full run including soak (15+ minutes)
  $0 --soak                 # Only soak test
  $0 --layer L2             # Only L2 (Art-Net)
  $0 --port /dev/ttyUSB1    # Use alternate serial port
  $0 --output results.html  # HTML report

EOF
}

# Parse arguments
RUN_MODE="quick"

while [[ $# -gt 0 ]]; do
    case $1 in
        --all)
            RUN_MODE="all"
            shift
            ;;
        --soak)
            RUN_MODE="soak"
            shift
            ;;
        --quick)
            RUN_MODE="quick"
            shift
            ;;
        --layer)
            RUN_MODE="layer"
            LAYER="$2"
            shift 2
            ;;
        --port)
            PORT="$2"
            shift 2
            ;;
        --device)
            DEVICE_IP="$2"
            shift 2
            ;;
        --output)
            OUTPUT_FILE="$2"
            shift 2
            ;;
        -v|--verbose)
            VERBOSE="-vv"
            shift
            ;;
        -q|--quiet)
            VERBOSE="-q"
            shift
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            print_usage
            exit 1
            ;;
    esac
done

# Check serial port
if [ ! -e "$PORT" ]; then
    echo -e "${YELLOW}WARNING: Serial port $PORT not found${NC}"
    echo "Available serial ports:"
    ls -la /dev/tty* 2>/dev/null || echo "  (none found)"
fi

# Set environment
export GLOW_SERIAL_PORT="$PORT"
export GLOW_DEVICE_IP="$DEVICE_IP"

echo -e "${GREEN}=== ESP-Glow HIL Test Runner ===${NC}"
echo "Serial Port: $PORT"
echo "Device IP:  $DEVICE_IP"
echo "Mode:       $RUN_MODE"
echo ""

# Build pytest command
PYTEST_CMD="pytest $VERBOSE"

if [ -n "$OUTPUT_FILE" ]; then
    PYTEST_CMD="$PYTEST_CMD --html=$OUTPUT_FILE --self-contained-html"
fi

# Run tests based on mode
case $RUN_MODE in
    quick)
        echo "Running L0-L6 (quick, no soak)..."
        $PYTEST_CMD -m "not slow" --tb=short
        ;;
    all)
        echo "Running all tests (including 10-minute soak)..."
        echo -e "${YELLOW}This will take 15+ minutes.${NC}"
        $PYTEST_CMD --tb=short
        ;;
    soak)
        echo "Running L5 soak test only (10 minutes)..."
        $PYTEST_CMD test_l5_soak.py --tb=short -s
        ;;
    layer)
        if [[ "$LAYER" =~ ^[Ll]([0-6])$ ]]; then
            LAYER_NUM="${BASH_REMATCH[1]}"
            echo "Running layer L${LAYER_NUM}..."
            $PYTEST_CMD test_l${LAYER_NUM}_*.py --tb=short
        else
            echo -e "${RED}Invalid layer: $LAYER (must be L0-L6)${NC}"
            exit 1
        fi
        ;;
esac

EXIT_CODE=$?

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
